<?php

/**
 * =======================================================================================
 * TIJDELIJKE VERGELIJKMODUS — profielfoto autocrop/face-detection
 * =======================================================================================
 * Doel: bij elke upload de verschillende manieren van gezichtsdetectie + bijsnijden
 * NAAST ELKAAR tonen, zodat we visueel kunnen kiezen welke aanpak het beste resultaat
 * geeft. De PRODUCTIE-foto (de ensemble-crop) verandert hierdoor NIET — dit draait op
 * een tijdelijke kopie van het origineel, vóór de echte crop.
 *
 * Per upload wordt een MONTAGE-afbeelding gemaakt en naar webteam@ gemaild met:
 *   - per detector (HOG / YuNet / MediaPipe): de volledige foto met de gevonden
 *     gezicht-box erop getekend  én  de resulterende vierkante crop;
 *   - de ensemble-winnaar met een matrix van crop-instellingen
 *     (padding 2.0× / 2.5× / 3.0×  ×  verticale centrering 42% / 50%).
 *
 * AAN/UIT (geen codewijziging nodig):
 *     drush vset ozk_profielfoto_vergelijk 1     # aan
 *     drush vdel ozk_profielfoto_vergelijk       # uit (of: vset ... 0)
 *
 * VERWIJDEREN later: dit .inc-bestand weghalen, de require_once + de twee
 * variable_get()-aanroepen uit ozk_profielfoto.module halen, en de variable vdel-en.
 */

/**
 * Bouwt de vergelijkings-montage voor één upload en mailt die naar webteam.
 *
 * @param string   $origineel_pad  Absoluut pad naar de ZOJUIST geüploade (nog niet
 *                                  gecropte) foto.
 * @param object   $bestand        Drupal file-object (voor de extensie/bestandsnaam).
 * @param int      $uid            Drupal user-id van de indiener.
 */
function _ozk_profielfoto_vergelijk_varianten($origineel_pad, $opts = array()) {
    try {
        // Identificatie: aanroepers geven óf een Drupal-uid (entityform/webform) óf
        // direct een CiviCRM contact_id (intake/CRM-profielroute). We leiden de
        // contact_id en een naam-string hieruit af voor de mail + bestandsnamen.
        $uid        = isset($opts['uid']) ? (int) $opts['uid'] : NULL;
        $contact_id = isset($opts['contact_id']) ? (int) $opts['contact_id'] : NULL;
        if (!$contact_id && $uid && civicrm_initialize()) {
            $contact_id = CRM_Core_BAO_UFMatch::getContactId($uid);
        }
        // Fallback: extraheer contact_id uit de OZK-bestandsnaam ([naam]_[cid]_[datum]).
        // Vangt gevallen op waarbij de UID niet in uf_match staat (drush, webteam-upload).
        if (!$contact_id && preg_match('/_(\d+)_\d{8}_\d{4}\./', basename($bron), $_m)) {
            $contact_id = (int) $_m[1];
        }
        $idstr = $contact_id ? ('cid' . $contact_id) : ($uid ? ('uid' . $uid) : 'x');

        $stamp   = date('Ymd_His');
        $tmpbase = drupal_realpath('temporary://');
        if (!$tmpbase) {
            $tmpbase = sys_get_temp_dir();
        }
        $werkmap = $tmpbase . '/ozkvgl_' . $idstr . '_' . $stamp;
        @mkdir($werkmap, 0775, TRUE);

        // 0. Werkkopie van het ORIGINEEL + orienteren (eerlijk: net als productie).
        $bron = $werkmap . '/bron.jpg';
        if (!@copy($origineel_pad, $bron)) {
            watchdog('ozk_profielfoto', 'vergelijk: kon origineel niet kopiëren: @p', ['@p' => $origineel_pad], WATCHDOG_WARNING);
            return;
        }
        exec('magick ' . escapeshellarg($bron . '[0]') . ' -auto-orient +repage -quality 95 ' . escapeshellarg($bron));

        // 1. Per-detector boxes ophalen via de --all-modus van ozk-facedetect.
        $script = dirname(__FILE__) . '/bin/ozk-facedetect';
        $json   = shell_exec(escapeshellarg($script) . ' ' . escapeshellarg($bron) . ' 0.08 --all');
        $data   = $json ? json_decode($json, TRUE) : NULL;
        if (!$data || empty($data['img_width'])) {
            watchdog('ozk_profielfoto', 'vergelijk: detector gaf geen bruikbare output', [], WATCHDOG_WARNING);
            return;
        }
        $W = (int) $data['img_width'];
        $H = (int) $data['img_height'];

        // 2. Productie-crop genereren — zelfde adaptieve logica als autocrop_op_pad.
        //    Gebruik $data['best'] (de winnaar na consensus over alle detectoren).
        $best_box   = $data['best'] ?? NULL;
        $prod_url   = NULL;
        $prod_label = 'geen gezicht gevonden';
        if ($best_box) {
            $bw       = $best_box['right'] - $best_box['left'];
            $bh       = $best_box['bottom'] - $best_box['top'];
            $gz_max   = max($bw, $bh);
            // Adaptief: gezicht = 35% van de crop-hoogte (zelfde als productie).
            $zijde    = (int) round($gz_max / 0.35);
            $zijde    = max($zijde, (int) round($gz_max * 1.5), 100);
            $zijde    = min($zijde, $W, $H);
            $cx       = (int) round(($best_box['left'] + $best_box['right'])  / 2);
            $cy       = (int) round(($best_box['top']  + $best_box['bottom']) / 2);
            $crop_top = max(0, min($cy - (int) round($zijde * 0.42), $H - $zijde));
            $crop_lft = max(0, min($cx - (int) round($zijde / 2),    $W - $zijde));

            // Hoofd-behoud — zelfde stap als _ozk_profielfoto_autocrop_op_pad sectie 3b,
            // anders toont deze preview een crop die productie niet meer maakt (was de
            // oorzaak van de Vera/Joshua-casus: hier stond nog de OUDE, kruin-afsnijdende
            // crop terwijl productie na de fix al ruimte boven het hoofd zou laten).
            $gz_l = $best_box['left']; $gz_t = $best_box['top'];
            $hoofd_top    = max(0, $gz_t - (int) round($bh * 0.6));
            $hoofd_bottom = min($H, $best_box['bottom'] + (int) round($bh * 0.15));
            $hoofd_left   = max(0, $gz_l - (int) round($bw * 0.2));
            $hoofd_right  = min($W, $best_box['right'] + (int) round($bw * 0.2));
            $hoofd_h = $hoofd_bottom - $hoofd_top; $hoofd_w = $hoofd_right - $hoofd_left;
            $hoofd_cx = (int) round(($hoofd_left + $hoofd_right) / 2);
            $hoofd_cy = (int) round(($hoofd_top  + $hoofd_bottom) / 2);
            $crop_bevat_hoofd = ($crop_top <= $hoofd_top && ($crop_top + $zijde) >= $hoofd_bottom
                                && $crop_lft <= $hoofd_left && ($crop_lft + $zijde) >= $hoofd_right);
            if (!$crop_bevat_hoofd) {
                $zijde    = min(max($zijde, $hoofd_h, $hoofd_w), $W, $H);
                $crop_lft = max(0, min($hoofd_cx - (int) round($zijde / 2), $W - $zijde));
                $crop_top = max(0, min($hoofd_cy - (int) round($zijde / 2), $H - $zijde));
            }
            $geom = ['zijde' => $zijde, 'left' => $crop_lft, 'top' => $crop_top];

            $prod_pad    = _ozk_vgl_crop($werkmap, $bron, $geom, 'prod');
            $_prod_pubdir = 'public://profielfoto-vergelijk';
            file_prepare_directory($_prod_pubdir, FILE_CREATE_DIRECTORY);
            $prod_uri = $_prod_pubdir . '/prod_' . $idstr . '_' . $stamp . '.jpg';
            $prod_new = file_unmanaged_copy($prod_pad, $prod_uri, FILE_EXISTS_REPLACE);
            $prod_url = $prod_new ? file_create_url($prod_new) : NULL;
            $prod_label = sprintf('productie-crop · %s · score %.2f · gezicht %d%%',
                strtoupper($best_box['detector'] ?? '?'),
                $best_box['score'] ?? 0,
                (int) round($gz_max / $zijde * 100));
        }

        // 3. Naar publieke map + origineel met ECHTE proporties (geen vierkant forceren).
        $pubdir = 'public://profielfoto-vergelijk';
        file_prepare_directory($pubdir, FILE_CREATE_DIRECTORY);

        // Origineel op ware verhouding — max 400px op langste zijde, geen crop.
        $orig_pad = $werkmap . '/orig_prop.jpg';
        exec('magick ' . escapeshellarg($bron . '[0]') . ' -resize 400x400 ' . escapeshellarg($orig_pad));
        $orig_uri = $pubdir . '/orig_' . $idstr . '_' . $stamp . '.jpg';
        $orig_new = file_unmanaged_copy($orig_pad, $orig_uri, FILE_EXISTS_REPLACE);
        $orig_url = $orig_new ? file_create_url($orig_new) : NULL;

        // 4. Montage: 4 kolommen per detector (box-overlay + crop 2.0/2.5/3.0).
        $cellen           = [];
        $padding_kolommen = [2.0, 2.5, 3.0];
        foreach ($data['detectors'] as $det) {
            $naam = strtoupper($det['name']);
            $box  = $det['largest'];
            if (!$box) {
                $reden = !empty($det['error']) ? 'overgeslagen' : 'geen gezicht';
                $cellen[] = ['path' => _ozk_vgl_leeg($werkmap, $det['name']),        'label' => "$naam — $reden"];
                for ($i = 0; $i < 3; $i++) {
                    $cellen[] = ['path' => _ozk_vgl_leeg($werkmap, $det['name'] . "_p$i"), 'label' => ''];
                }
                continue;
            }
            $cellen[] = [
                'path'  => _ozk_vgl_overlay($werkmap, $bron, $box, $W, $det['name'] . '_box'),
                'label' => sprintf('%s  %.2f  box', $naam, $box['score']),
            ];
            foreach ($padding_kolommen as $padding) {
                $g   = _ozk_vgl_geom($box, $padding, 0.42, $W, $H);
                $key = sprintf('%s_p%02d', $det['name'], (int) round($padding * 10));
                $cellen[] = [
                    'path'  => _ozk_vgl_crop($werkmap, $bron, $g, $key),
                    'label' => sprintf('%s  %.1f×', $naam, $padding),
                ];
            }
        }
        $montage    = $werkmap . '/montage.jpg';
        $cmd_montage = 'magick montage';
        foreach ($cellen as $c) {
            if (!empty($c['path']) && is_file($c['path'])) {
                $cmd_montage .= ' -label ' . escapeshellarg($c['label']) . ' ' . escapeshellarg($c['path']);
            }
        }
        $cmd_montage .= ' -font DejaVu-Sans-Bold -pointsize 19 -tile 4x -geometry 320x320+10+12'
                     . ' -background white -fill black ' . escapeshellarg($montage);
        exec($cmd_montage, $uit_m, $rv_m);
        $montage_uri = $pubdir . '/vgl_' . $idstr . '_' . $stamp . '.jpg';
        $montage_new = ($rv_m === 0 && is_file($montage))
            ? file_unmanaged_copy($montage, $montage_uri, FILE_EXISTS_REPLACE) : NULL;
        $montage_url = $montage_new ? file_create_url($montage_new) : NULL;

        // 5. Naam (+ DITJAAR kampkort) + tekst-tabel met de ruwe cijfers, dan mailen.
        $naamstr  = '';
        $kampkort = '';
        if ($contact_id && civicrm_initialize()) {
            try {
                $r = civicrm_api4('Contact', 'get', [
                    'checkPermissions' => FALSE,
                    'select'           => ['display_name'],
                    'where'            => [['id', '=', $contact_id]],
                ]);
                $naamstr = $r->first()['display_name'] ?? '';
            }
            catch (Exception $e) {
                // naam optioneel
            }
            // DITJAAR kampkort (bv. 'TK2') uit de inschrijving van dit jaar, als die er is.
            // Zelfde bron als verwerk_na_move; route-onafhankelijk via base_find_allpart.
            if (function_exists('base_find_allpart')) {
                $allpart  = base_find_allpart($contact_id);
                $kampkort = ($allpart && !empty($allpart['result_allpart_one_kampkort']))
                    ? $allpart['result_allpart_one_kampkort'] : '';
            }
        }
        // Weergavetitel: "Naam (TK2)" als kampkort bekend is, anders alleen naam/id.
        $titelstr = ($naamstr ?: $idstr) . ($kampkort ? ' (' . $kampkort . ')' : '');

        $rijen = '';
        foreach ($data['detectors'] as $det) {
            $b = $det['largest'];
            $cel = $b
                ? sprintf('box %d×%d @ (%d,%d) — score %.2f',
                    $b['right'] - $b['left'], $b['bottom'] - $b['top'], $b['left'], $b['top'], $b['score'])
                : (!empty($det['error']) ? 'overgeslagen (' . htmlspecialchars(substr($det['error'], 0, 60)) . ')' : 'geen gezicht');
            $rijen .= sprintf('<tr><td><b>%s</b></td><td>%s ms</td><td>%s</td></tr>',
                strtoupper($det['name']), isset($det['ms']) ? $det['ms'] : '?', $cel);
        }

        $tabel = '<table cellpadding="5" style="border-collapse:collapse;border:1px solid #ccc">'
               . '<tr style="background:#f0f0f0"><th align="left">detector</th><th align="left">tijd</th><th align="left">gevonden</th></tr>'
               . $rijen . '</table>';

        // Upload-links onderaan, mét geldige CiviCRM-checksum voor déze contact.
        $links_html = '';
        if ($contact_id && civicrm_initialize()) {
            $cs     = CRM_Contact_BAO_Contact_Utils::generateChecksum($contact_id);
            $q      = '?cid=' . $contact_id . '&cs=' . $cs;
            $civi_q = '?reset=1&gid=143&id=' . $contact_id . '&cs=' . $cs;
            $links_html =
                '<p style="margin-top:16px"><b>Upload nieuwe foto via:</b></p><ol>'
              . '<li><a href="https://www.onvergetelijk.nl/profielfoto/edit' . $q . '">https://www.onvergetelijk.nl/profielfoto/edit</a> — entityform</li>'
              . '<li><a href="https://www.onvergetelijk.nl/civicrm/profile/edit' . $civi_q . '">CiviCRM profiel 143</a> — civicrm/profile/edit</li>'
              . '<li><a href="https://www.onvergetelijk.nl/form/profielfoto' . $q . '">https://www.onvergetelijk.nl/form/profielfoto</a> — webform upload</li>'
              . '</ol>';
        }

        // Twee foto's naast elkaar: origineel links, productie-crop rechts.
        $fotos_html = '<table cellpadding="0" cellspacing="0" style="margin-top:16px"><tr>'
            . '<td valign="top" style="padding-right:24px">'
            . ($orig_url
                ? '<img src="' . $orig_url . '" style="display:block;max-width:320px;border:1px solid #ddd">'
                  . '<div style="font-size:12px;color:#888;margin-top:4px">origineel ' . $W . '×' . $H . '</div>'
                : '<i>(origineel niet beschikbaar)</i>')
            . '</td>'
            . '<td valign="top">'
            . ($prod_url
                ? '<img src="' . $prod_url . '" style="display:block;max-width:320px;border:1px solid #ddd">'
                  . '<div style="font-size:12px;color:#888;margin-top:4px">' . htmlspecialchars($prod_label) . '</div>'
                : '<i>' . htmlspecialchars($prod_label) . '</i>')
            . '</td>'
            . '</tr></table>';

        $montage_html = $montage_url
            ? '<p style="margin-top:20px"><img src="' . $montage_url . '" style="max-width:100%;border:1px solid #ddd"></p>'
            : '<p style="color:#c00">(montage niet beschikbaar)</p>';

        $html = '<p>Vergelijking foto-autocrop voor <b>' . htmlspecialchars($titelstr) . '</b> — ' . $stamp . '</p>'
              . $tabel
              . $fotos_html
              . $montage_html
              . $links_html
              . '<p style="color:#888;font-size:12px">Tijdelijke vergelijkmodus. Uitzetten: <code>drush vdel ozk_profielfoto_vergelijk</code></p>';

        if (civicrm_initialize()) {
            // CRM_Utils_Mail::send() neemt $params by reference → eerst in een variabele.
            $mailparams = [
                'from'    => 'Onvergetelijke Zomerkampen <info@onvergetelijk.nl>',
                'toEmail' => 'webteam@onvergetelijk.nl',
                'toName'  => 'Webteam OZK',
                'subject' => '[VERGELIJK] foto-autocrop ' . $titelstr . ' ' . $stamp,
                'html'    => $html,
            ];
            CRM_Utils_Mail::send($mailparams);
        }

        watchdog('ozk_profielfoto', 'vergelijk gemaild voor @u: orig=@o prod=@p montage=@m',
            ['@u' => $idstr, '@o' => $orig_url ?: '(geen)', '@p' => $prod_url ?: '(geen)', '@m' => $montage_url ?: '(geen)'], WATCHDOG_INFO);

        // Werkmap opruimen (montage staat al in public://).
        if (function_exists('file_unmanaged_delete_recursive')) {
            file_unmanaged_delete_recursive($werkmap);
        }
    }
    catch (Exception $e) {
        watchdog('ozk_profielfoto', 'vergelijk faalde: @m', ['@m' => $e->getMessage()], WATCHDOG_WARNING);
    }
}

/**
 * Bereken de vierkante crop-geometrie rond een box — identiek aan de productie-math in
 * _ozk_profielfoto_autocrop_op_pad(), zodat de getoonde crop exact is wat productie
 * met die instelling zou maken.
 *
 * @return array  ['zijde' => .., 'left' => .., 'top' => ..]
 */
function _ozk_vgl_geom($box, $padding, $center, $W, $H) {
    $bw = $box['right'] - $box['left'];
    $bh = $box['bottom'] - $box['top'];
    $cx = (int) round(($box['left'] + $box['right']) / 2);
    $cy = (int) round(($box['top'] + $box['bottom']) / 2);

    $zijde = (int) round(max($bw, $bh) * $padding);
    $zijde = max($zijde, 100);
    $zijde = min($zijde, $W, $H);

    $top  = $cy - (int) round($zijde * $center);
    $left = $cx - (int) round($zijde / 2);
    $left = max(0, min($left, $W - $zijde));
    $top  = max(0, min($top, $H - $zijde));

    return ['zijde' => $zijde, 'left' => $left, 'top' => $top];
}

/**
 * Cel: volledige foto met de gevonden gezicht-box er rood op getekend, geschaald.
 */
function _ozk_vgl_overlay($werkmap, $bron, $box, $W, $key) {
    $out  = $werkmap . '/cel_' . $key . '.jpg';
    $sw   = max(3, (int) round($W / 250)); // strokebreedte schaalt mee met de bron
    $rect = sprintf('rectangle %d,%d %d,%d', $box['left'], $box['top'], $box['right'], $box['bottom']);
    exec('magick ' . escapeshellarg($bron . '[0]')
        . ' -strokewidth ' . $sw . ' -stroke red -fill none -draw ' . escapeshellarg($rect)
        . ' -resize 320x320 ' . escapeshellarg($out));
    return $out;
}

/**
 * Cel: de resulterende vierkante crop, geschaald.
 */
function _ozk_vgl_crop($werkmap, $bron, $g, $key) {
    $out  = $werkmap . '/cel_' . $key . '.jpg';
    $geom = sprintf('%dx%d+%d+%d', $g['zijde'], $g['zijde'], $g['left'], $g['top']);
    exec('magick ' . escapeshellarg($bron . '[0]')
        . ' -crop ' . escapeshellarg($geom) . ' +repage -resize 320x320 ' . escapeshellarg($out));
    return $out;
}

/**
 * Cel: de volledige (georiënteerde) originele foto, ongecropt, op schaal.
 */
function _ozk_vgl_origineel($werkmap, $bron, $key) {
    $out = $werkmap . '/cel_' . $key . '.jpg';
    exec('magick ' . escapeshellarg($bron . '[0]') . ' -resize 320x320 ' . escapeshellarg($out));
    return $out;
}

/**
 * Cel: grijze placeholder voor een detector die niets vond / werd overgeslagen.
 */
function _ozk_vgl_leeg($werkmap, $key) {
    $out = $werkmap . '/cel_leeg_' . $key . '.jpg';
    exec('magick -size 320x320 xc:#eeeeee ' . escapeshellarg($out));
    return $out;
}
