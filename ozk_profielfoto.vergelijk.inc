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

        // 2. Cellen bouwen — ÉÉN RIJ PER METHODE, 4 cellen per rij:
        //    [box-overlay] [crop 2.0×] [crop 2.5×] [crop 3.0×]  (centrum 42%).
        //    Met -tile 4x levert dat per detector precies één regel op. Een detector
        //    die niets vond krijgt een placeholder + 3 lege cellen zodat het raster
        //    netjes uitgelijnd blijft. Het origineel staat NIET in het grid maar
        //    bovenaan de mail naast de tabel.
        $cellen           = [];
        $padding_kolommen = [2.0, 2.5, 3.0];

        foreach ($data['detectors'] as $det) {
            $naam = strtoupper($det['name']);
            $box  = $det['largest'];

            if (!$box) {
                $reden = !empty($det['error']) ? 'overgeslagen' : 'geen gezicht';
                $cellen[] = ['path' => _ozk_vgl_leeg($werkmap, $det['name']), 'label' => sprintf('%s — %s', $naam, $reden)];
                for ($i = 0; $i < 3; $i++) {
                    $cellen[] = ['path' => _ozk_vgl_leeg($werkmap, $det['name'] . '_pad' . $i), 'label' => ''];
                }
                continue;
            }

            // Kolom 1: volledige foto met de gevonden box.
            $cellen[] = [
                'path'  => _ozk_vgl_overlay($werkmap, $bron, $box, $W, $det['name'] . '_box'),
                'label' => sprintf('%s  %.2f  ·  box', $naam, $box['score']),
            ];
            // Kolom 2-4: de crop bij drie paddings (centrum 42%).
            foreach ($padding_kolommen as $padding) {
                $g   = _ozk_vgl_geom($box, $padding, 0.42, $W, $H);
                $key = sprintf('%s_p%02d', $det['name'], (int) round($padding * 10));
                $cellen[] = [
                    'path'  => _ozk_vgl_crop($werkmap, $bron, $g, $key),
                    'label' => sprintf('%s  ·  crop %.1f×', $naam, $padding),
                ];
            }
        }

        // 3. Montage samenstellen (montage zet de labels netjes onder elke tegel).
        $montage = $werkmap . '/montage.jpg';
        $cmd = 'magick montage';
        foreach ($cellen as $c) {
            if (empty($c['path']) || !is_file($c['path'])) {
                continue;
            }
            $cmd .= ' -label ' . escapeshellarg($c['label']) . ' ' . escapeshellarg($c['path']);
        }
        // Duidelijke, schreefloze labels: groot, vet, zwart op wit. DejaVu-Sans-Bold is
        // op deze server beschikbaar (anders valt montage terug op de standaard).
        $titel = 'OZK foto-autocrop vergelijking — ' . $stamp;
        $cmd .= ' -font DejaVu-Sans-Bold -pointsize 19 -tile 4x -geometry 380x380+14+16'
              . ' -background white -fill black -title ' . escapeshellarg($titel)
              . ' ' . escapeshellarg($montage);
        exec($cmd, $uit, $rv);
        if ($rv !== 0 || !is_file($montage)) {
            watchdog('ozk_profielfoto', 'vergelijk: montage mislukt (rv=@r)', ['@r' => $rv], WATCHDOG_WARNING);
            return;
        }

        // 4. Naar publieke map (zodat <img> in de mail rendert) + opruimen werkmap.
        $pubdir = 'public://profielfoto-vergelijk';
        file_prepare_directory($pubdir, FILE_CREATE_DIRECTORY);
        $puburi = $pubdir . '/vgl_' . $idstr . '_' . $stamp . '.jpg';
        $nieuw  = file_unmanaged_copy($montage, $puburi, FILE_EXISTS_REPLACE);
        $url    = $nieuw ? file_create_url($nieuw) : NULL;

        // Los origineel-thumbnail voor bovenin de mail (naast de tabel).
        $orig_cel = _ozk_vgl_origineel($werkmap, $bron, 'mailorig');
        $orig_uri = $pubdir . '/orig_' . $idstr . '_' . $stamp . '.jpg';
        $orig_new = file_unmanaged_copy($orig_cel, $orig_uri, FILE_EXISTS_REPLACE);
        $orig_url = $orig_new ? file_create_url($orig_new) : NULL;

        // 5. Naam + tekst-tabel met de ruwe cijfers, dan mailen.
        $naamstr = '';
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
        }

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

        $orig_html = $orig_url
            ? '<img src="' . $orig_url . '" style="display:block;max-width:320px;border:1px solid #ddd">'
              . '<div style="font-size:12px;color:#888;margin-top:4px">origineel ' . $W . '×' . $H . '</div>'
            : '';

        // Upload-links onderaan, mét geldige CiviCRM-checksum voor déze contact, zodat
        // de ontvanger direct (zonder inloggen) een nieuwe foto kan uploaden ter test.
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

        // Bovenin: tabel links, origineel rechts (layout-tabel = betrouwbaar in mailclients).
        $html = '<p>Vergelijking foto-autocrop voor <b>' . htmlspecialchars($naamstr ?: $idstr) . '</b> — ' . $stamp . '</p>'
              . '<table cellpadding="0" cellspacing="0"><tr>'
              . '<td valign="top">' . $tabel . '</td>'
              . '<td valign="top" style="padding-left:24px">' . $orig_html . '</td>'
              . '</tr></table>'
              . ($url ? '<p style="margin-top:16px"><img src="' . $url . '" style="max-width:100%;border:1px solid #ddd"></p>' : '<p>(montage niet beschikbaar)</p>')
              . $links_html
              . '<p style="color:#888;font-size:12px">Tijdelijke vergelijkmodus. Uitzetten: <code>drush vdel ozk_profielfoto_vergelijk</code></p>';

        if (civicrm_initialize()) {
            // CRM_Utils_Mail::send() neemt $params by reference → eerst in een variabele.
            $mailparams = [
                'from'    => 'OZK <info@onvergetelijk.nl>',
                'toEmail' => 'webteam@onvergetelijk.nl',
                'toName'  => 'Webteam OZK',
                'subject' => '[VERGELIJK] foto-autocrop ' . ($naamstr ?: $idstr) . ' ' . $stamp,
                'html'    => $html,
            ];
            CRM_Utils_Mail::send($mailparams);
        }

        watchdog('ozk_profielfoto', 'vergelijk-montage gemaild voor uid @u (@n cellen): @url',
            ['@u' => $idstr, '@n' => count($cellen), '@url' => $url], WATCHDOG_INFO);

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
