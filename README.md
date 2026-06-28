# ozk_profielfoto

Drupal 7-module die profielfoto's van OZK-leiding/deelnemers synchroniseert naar
CiviCRM en zorgt dat ze op het account-dashboard verschijnen.

## Functionele beschrijving

Een gebruiker uploadt zijn profielfoto via het **entityform `profielfoto`**
(`/eform/submit/profielfoto`). Deze module pakt die inzending op en zet de
foto-URL op het bijbehorende CiviCRM-contact, zodat de foto zowel op de
CiviCRM-contactkaart als op het account-dashboard zichtbaar is.

## Werking

| Hook / functie | Rol |
|---|---|
| `ozk_profielfoto_entity_insert` / `_update` | Vuren bij opslaan van een `profielfoto`-entityform → roepen de sync aan |
| `_ozk_profielfoto_sync_profielfoto_naar_civicrm($entity)` | Haalt `field_profielfoto`-bestand op, maakt er een absolute URL van (`file_create_url`) en delegeert |
| `_ozk_profielfoto_push_image_url_naar_civicrm($uid, $image_url)` | Testbare seam: zoekt contact bij Drupal-uid, schrijft `image_URL` weg via APIv4 en verstuurt notificatiemail |
| `ozk_profielfoto_form_alter` + `_..._redirect_force` | Forceren redirect terug naar `/account` na upload |

De APIv4 `Contact.update` met enkel `image_URL` triggert vervolgens de
`intake_civicrm_pre`-hook in **nl.onvergetelijk.intake**, die `FOT_update` en
`FOT_status` bijwerkt. Deze module schrijft die statusvelden dus bewust **niet**
zelf — zie [`nl.onvergetelijk.intake`](../civicrm_extensions/nl.onvergetelijk.intake/README.md).

## Notificatiemail

Na een succesvolle sync stuurt `_ozk_profielfoto_push_image_url_naar_civicrm()` een
HTML-notificatie (naar de hardcoded beheerder). Opbouw:

- **Contact ID**, daaronder de **displaynaam op een eigen regel**, en (indien
  bekend) **kamp + functie** via `base_find_allpart()`.
- Een regel dat de foto **via het entityform** is geüpload (onderscheidt zich van
  een native CiviCRM-upload — daarvoor vuurt deze module niet).
- De **profielfoto inline** (zie hieronder) en onderaan een link
  `» Open contactrecord in CiviCRM` (`civicrm/contact/view?reset=1&cid=…`).

**Foto inline i.p.v. hotlink (`cid:`).** Mailclients (Gmail) proxyen/blokkeren
vaak een externe `<img src="https://…">`, waardoor je een placeholder i.p.v. de
foto ziet. Daarom stuurt de mail de foto **inline mee** als MIME-bijlage
(`multipart/related` + `Content-ID: <profielfoto>`, base64): de afbeelding zit ín
de mail, dus de client hoeft niets extern op te halen → altijd zichtbaar. Het
lokale bestandspad wordt uit de absolute URL afgeleid (web-root + pad; een
`/styles/<preset>/public`-segment wordt defensief gestript). Is het bestand niet
leesbaar, dan valt de mail terug op een enkelvoudige HTML-mail met hotlink-`<img>`.

> Verzending loopt via **msmtp** (`sendmail_path = /usr/bin/msmtp -t`, relay
> `smtp.gmail.com`, `from info@onvergetelijk.nl` met `auto_from off`) — log in
> `/var/log/msmtp.log`. De afzender wordt dus op envelope-niveau altijd
> `info@onvergetelijk.nl` (SPF/DKIM-conform); de oude nep-`From: …@ozk_profielfoto.nl`
> in de header is verwijderd.

Gedekt door unit-tests (`OzkProfielfotoTest`): mailinhoud, **cid-embed bij bestaand
bestand** en **hotlink-fallback bij ontbrekend bestand**.

## Twee opslag- en renderpaden voor profielfoto's (belangrijk)

Er bestaan op deze site **twee** manieren waarop een contact aan een foto komt.
Ze worden verschillend opgeslagen én verschillend gerenderd — dit is de bron van
de meeste foto-verwarring:

| Pad | Opslag | `image_URL`-vorm | Dashboard-rendering |
|---|---|---|---|
| **A. Entityform** (deze module) | Drupal public files: `sites/default/files/profielfotos/<naam>_<drupaluid>_<datum>.jpg` | absolute Drupal-bestands-URL | image-style `square_profielfoto` (Drupal) → werkt frontend |
| **B. Native CiviCRM** (contact → foto bewerken) | CiviCRM private dir: `private/civicrm/custom/<bestand>.jpg` | `…/civicrm/contact/imagefile?photo=<bestand>.jpg` | **alleen** via de CiviCRM imagefile-route |

Het account-dashboard wordt opgebouwd door de Drupal-view
`civicrm_contact` → display **`block_profiel`**:

1. Primair toont die de **embedded view `user_entityforms:ephoto_url_uid`**
   (pad A: entityform-foto via image-style). Geeft rijen terug zodra er een
   `profielfoto`-entityform is.
2. Heeft de gebruiker **geen** entityform-inzending, dan valt het blok terug op
   de *empty text* `<img src="[image_URL]">` — dat is pad B, de imagefile-route.

> ⚠️ **Pad B kan alleen renderen als de CiviCRM imagefile-route werkt voor
> frontend-bezoekers.** Zie hieronder.

## Valkuil: imagefile-route 404 → `CIVICRM_CLEANURL`

De route `civicrm/contact/imagefile?photo=…` wordt afgehandeld door core
`CRM_Contact_Page_ImageFile`. Die zoekt het contact via een **exacte match** op
`image_url` met een door `CRM_Utils_System::url()` herbouwde URL (de "M61"-fix).

`CRM_Utils_System::url()` levert echter **alleen de clean-URL-vorm** als
`config->cleanURL == 1`. Zonder de constante `CIVICRM_CLEANURL` valt die in het
normale web-invoke-pad terug op `0`, waardoor de functie de **non-clean** vorm
`…/index.php?q=civicrm/contact/imagefile&photo=…` bouwt. Die matcht nooit de
clean-URL in de database → `cid = NULL` → **HTTP 404 op álle ~1510
imagefile-foto's bij elke frontend-request** (account-dashboard, smoelenboek).
De CiviCRM-contactkaart lijkt dan nog te werken, maar dat is **browsercache**
(12 u TTL), geen echte werking.

**Fix (toegepast 2026-06-22)** in `sites/default/civicrm.settings.php`, vlak na
`CIVICRM_UF_BASEURL`:

```php
if (!defined('CIVICRM_CLEANURL')) {
	define('CIVICRM_CLEANURL', 1);
}
```

Dit staat in het settings-bestand (niet in core) en overleeft dus
CiviCRM-upgrades. De regressietest
`Civi\OzkTest\Security\ImagefileRouteTest` (extensie `nl.onvergetelijk.test`)
bewaakt dat de route 200 + een afbeelding blijft geven.

## Autocrop / gezichtsdetectie (`bin/ozk-facedetect`)

Bij elke upload snijdt `_ozk_profielfoto_autocrop_op_pad()` de foto vierkant bij
rondom het gezicht. De flow is hard afgesteld tegen twee echte fouten (28-jun-2026):

1. **EXIF-orientatie wordt eerst in het bestand gebakken** (sectie 0). Detectie,
   geometrie én de uiteindelijke crop werken zo in dezelfde, rechtopstaande
   pixelruimte. Zonder dit kreeg een gekantelde telefoonfoto een verkeerde crop en
   miste de detector het zijwaartse gezicht (→ false positive in de achtergrond).
2. **Plausibiliteitsguard**: een "gezicht" kleiner dan `$min_face_ratio` (0.08) ×
   de korte zijde is vrijwel zeker een false positive → de foto wordt **onaangeroerd**
   gelaten (de vierkante image-styles maken alsnog een gecentreerd vierkant). Ook
   "geen gezicht gevonden" laat de foto heel i.p.v. te gokken.

`bin/ozk-facedetect` draait detectoren **parallel** (één Python-proces, ThreadPool)
en kiest het door consensus bevestigde gezicht (overlap van ≥2 engines wint; daarna
grootste oppervlak). Detectie loopt op een verkleinde kopie (`DETECT_MAX=1600`); de
crop gebeurt op het volle origineel → geen kwaliteitsverlies, wél veel sneller.

| Engine | Sterkte | Rol |
|---|---|---|
| OpenCV **YuNet** (ONNX, CPU) | robuustste all-rounder, ~1-2s | **productie (primair)** |
| `face_recognition` (dlib **HOG**) | goede tweede stem, frontaal | **productie (consensus)** |
| `face_recognition` (dlib **CNN**, mmod) | sterk op gedraaid/scheef, maar traag (~9s) | **vangnet + vergelijking** |

**Productie-cascade** (alle 3 routes): YuNet + HOG parallel (~2-3s); **CNN alleen
als vangnet** wanneer die twee niets bruikbaars vinden. Modellen lokaal in `bin/`
(`face_detection_yunet_2023mar.onnx`; CNN-model zit in het `face_recognition`-pakket).
Venv: `/home/webteam/venv/facedetect` (py3.13).

> **MediaPipe is verwijderd** (28-jun-2026): het BlazeFace short-range model faalde
> structureel op afstands-/ten-voeten-uit-foto's en koos in een multi-face-foto de
> verkeerde persoon. Pakket ge-uninstalld uit de venv en model van schijf gehaald;
> YuNet/HOG/CNN dekken de gevallen ruimschoots.

### `--all`-modus (vergelijking)

`ozk-facedetect <pad> [min_ratio] --all` geeft **élke** detector apart terug
(grootste box + score + timing) plus de ensemble-winnaar — nu YuNet/HOG/CNN.
Gebruikt door de tijdelijke vergelijkmodus hieronder.

### Tijdelijke vergelijkmodus

Voor het afstellen kun je bij **elke upload** een montage laten mailen met alle
detector- én crop-varianten naast elkaar (volledige foto + getekende box én de
resulterende crop; plus een matrix padding 2.0/2.5/3.0× × centrering 42/50% op de
ensemble-winnaar). Code: `ozk_profielfoto.vergelijk.inc`. De productie-crop blijft
ongewijzigd (de vergelijking draait op een tijdelijke kopie van het origineel).

```bash
drush vset ozk_profielfoto_vergelijk 1   # aan  → montage-mail naar webteam@ per upload
drush vdel ozk_profielfoto_vergelijk     # uit
```

Montages komen in `public://profielfoto-vergelijk/`. Verwijderen later: het
`.inc`-bestand + de twee `variable_get('ozk_profielfoto_vergelijk', …)`-aanroepen
in de upload-paden weghalen.

## Webform-route (node 608) → publieke master

Naast pad A (entityform) en B (native CiviCRM) bestaat een **webform-upload**
(`/form/profielfoto`, node 608, `_ozk_profielfoto_webform_foto_verwerken()`). De
webform-filecomponent slaat op in `private://` → de URL wordt `/system/files/…`
(toegangs-gecontroleerd: 403/404 voor anonieme bezoekers en mailclients → blanco
foto). Daarom kopieert `_ozk_profielfoto_kopieer_naar_public_master()` de gecropte
foto naar de **publieke** master-map `public://profielfotos/<slug>_<cid>_<datum>.<ext>`
(gelijk aan pad A) en wordt **die** URL naar CiviCRM gepusht.

## Tests

Unit-tests voor de module-functies draaien standalone (gestubde Drupal/CiviCRM):

- `nl.onvergetelijk.intake/tests/phpunit/unit/OzkProfielfotoTest.php` (suite
  `phpunit.unit.xml`, bootstrap `bootstrap.unit.php`).
- E2E sync-keten: `nl.onvergetelijk.intake/tests/phpunit/Civi/Intake/OzkProfielfotoSyncE2ETest.php`.

```bash
cd /var/www/vhosts/ozkprod/web/sites/all/modules/civicrm_extensions/nl.onvergetelijk.intake
phpunit9 --configuration=phpunit.unit.xml
```

---

*Onderdeel van de OZK-suite. Beheerd door Stichting Onvergetelijke Zomerkampen.*
