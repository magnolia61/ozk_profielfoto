<?php

use PHPUnit\Framework\TestCase;

/**
 * Tests voor de HEIC/HEIF -> JPEG conversie bij profielfoto-uploads.
 *
 * Dekt twee functies uit ozk_profielfoto.module:
 *   - _ozk_profielfoto_validate_heic_naar_jpg()  (de upload-validator/converter)
 *   - _ozk_profielfoto_heic_validator_inhaken()   (injectie in #upload_validators)
 *
 * De conversie zelf heeft de 'magick'-binary (libheif) nodig; die tests worden
 * overgeslagen als magick of HEIC-write-support ontbreekt.
 *
 * Draaien:
 *   cd sites/all/modules/ozk_profielfoto
 *   phpunit9 --configuration=phpunit.xml.dist
 */
class HeicConversieTest extends TestCase {

    /** @var string Tijdelijke werkmap voor fixtures. */
    private $werkmap;

    protected function setUp(): void {
        $this->werkmap = sys_get_temp_dir() . '/ozk_heic_test_' . uniqid('', TRUE);
        mkdir($this->werkmap, 0777, TRUE);
        // Reset de watchdog-stublog per test.
        $GLOBALS['_test_watchdog_log'] = [];
    }

    protected function tearDown(): void {
        foreach (glob($this->werkmap . '/*') as $f) {
            @unlink($f);
        }
        @rmdir($this->werkmap);
    }

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    private function magickAanwezig(): bool {
        exec('magick -version 2>/dev/null', $uit, $rv);
        return $rv === 0;
    }

    /** Maakt een echte HEIC-testafbeelding; skipt als magick HEIC niet kan schrijven. */
    private function maakHeicFixture(string $pad, string $size = '1200x1600'): void {
        if (!$this->magickAanwezig()) {
            $this->markTestSkipped('magick-binary niet beschikbaar.');
        }
        exec('magick -size ' . escapeshellarg($size) . ' gradient:blue-white '
             . escapeshellarg($pad) . ' 2>/dev/null', $uit, $rv);
        if ($rv !== 0 || !is_file($pad) || filesize($pad) === 0) {
            $this->markTestSkipped('magick kan geen HEIC schrijven (libheif ontbreekt?).');
        }
        // Bevestig dat het echt een HEIC is die GD/getimagesize NIET kan lezen.
        if (@getimagesize($pad) !== FALSE) {
            $this->markTestSkipped('Gegenereerde fixture is geen HEIC (door getimagesize leesbaar).');
        }
    }

    /** Bouwt een $file-object zoals file_save_upload dat tijdens validatie aanlevert. */
    private function maakFileObject(string $uri, string $filename, string $mime): stdClass {
        $file = new stdClass();
        $file->uri      = $uri;
        $file->filename = $filename;
        $file->filemime = $mime;
        $file->filesize = is_file($uri) ? filesize($uri) : 0;
        return $file;
    }

    private function laatsteWatchdog(): ?array {
        $log = $GLOBALS['_test_watchdog_log'];
        return empty($log) ? NULL : end($log);
    }

    // -----------------------------------------------------------------------
    // Conversie-tests (validator)
    // -----------------------------------------------------------------------

    public function testHeicWordtNaarJpegGeconverteerd(): void {
        $bron = $this->werkmap . '/sample.heic';
        $this->maakHeicFixture($bron);

        // Simuleer een tmp-upload: kaal pad zonder extensie (zoals PHP doet).
        $tmp = $this->werkmap . '/php_upload_tmp';
        copy($bron, $tmp);

        $file   = $this->maakFileObject($tmp, 'IMG_2073.heic', 'image/heic');
        $errors = _ozk_profielfoto_validate_heic_naar_jpg($file);

        $this->assertSame([], $errors, 'Een geldige HEIC mag geen validatiefouten geven.');
        $this->assertMatchesRegularExpression('/\.jpg$/i', $file->filename, 'Bestandsnaam moet .jpg worden.');
        $this->assertSame('image/jpeg', $file->filemime);

        // De tmp-file is in place vervangen door een echte JPEG.
        $info = getimagesize($tmp);
        $this->assertNotFalse($info, 'Na conversie moet getimagesize de file kunnen lezen.');
        $this->assertSame('image/jpeg', $info['mime']);
        $this->assertSame(1200, $info[0], 'Breedte moet behouden blijven.');
        $this->assertSame(1600, $info[1], 'Hoogte moet behouden blijven.');
    }

    public function testHeicHerkendViaExtensieOokAlsHeaderAfwijkt(): void {
        // Echte HEIC-bytes, maar met een .heif-naam: detectie via ftyp-merk EN extensie.
        $bron = $this->werkmap . '/sample.heic';
        $this->maakHeicFixture($bron);
        $tmp = $this->werkmap . '/php_upload_tmp2';
        copy($bron, $tmp);

        $file   = $this->maakFileObject($tmp, 'foto.HEIF', 'application/octet-stream');
        $errors = _ozk_profielfoto_validate_heic_naar_jpg($file);

        $this->assertSame([], $errors);
        $this->assertMatchesRegularExpression('/\.jpg$/i', $file->filename);
        $this->assertNotFalse(getimagesize($tmp));
    }

    public function testGewoneJpegBlijftOngemoeid(): void {
        if (!$this->magickAanwezig()) {
            $this->markTestSkipped('magick-binary niet beschikbaar.');
        }
        $jpg = $this->werkmap . '/gewoon.jpg';
        exec('magick -size 800x800 gradient:red-yellow ' . escapeshellarg($jpg) . ' 2>/dev/null');
        $this->assertFileExists($jpg);

        $tmp = $this->werkmap . '/php_upload_jpg';
        copy($jpg, $tmp);
        $md5_voor = md5_file($tmp);

        $file   = $this->maakFileObject($tmp, 'foto.jpg', 'image/jpeg');
        $errors = _ozk_profielfoto_validate_heic_naar_jpg($file);

        $this->assertSame([], $errors);
        $this->assertSame('foto.jpg', $file->filename, 'Naam van een gewone JPEG mag niet wijzigen.');
        $this->assertSame($md5_voor, md5_file($tmp), 'De JPEG-bytes mogen niet worden aangeraakt.');
    }

    public function testMislukteConversieGeeftFoutmeldingEnLaatNaamStaan(): void {
        // Bestand met .heic-naam maar onzin-inhoud -> magick faalt -> foutpad.
        $tmp = $this->werkmap . '/kapot_upload';
        file_put_contents($tmp, 'dit is absoluut geen plaatje');

        $file   = $this->maakFileObject($tmp, 'kapot.heic', 'image/heic');
        $errors = _ozk_profielfoto_validate_heic_naar_jpg($file);

        $this->assertNotEmpty($errors, 'Een onverwerkbare HEIC moet een validatiefout opleveren.');
        $this->assertSame('kapot.heic', $file->filename, 'Bij falen blijft de naam ongewijzigd (early return).');

        $wd = $this->laatsteWatchdog();
        $this->assertNotNull($wd);
        $this->assertSame('ozk_profielfoto', $wd['type']);
        $this->assertSame(WATCHDOG_ERROR, $wd['severity']);
    }

    public function testOnbekendBestandZonderHeicWordtGenegeerd(): void {
        // Geen HEIC, geen .heic-extensie -> validator doet niets, geen fouten.
        $tmp = $this->werkmap . '/willekeurig.txt';
        file_put_contents($tmp, 'platte tekst');

        $file   = $this->maakFileObject($tmp, 'document.txt', 'text/plain');
        $errors = _ozk_profielfoto_validate_heic_naar_jpg($file);

        $this->assertSame([], $errors);
        $this->assertSame('document.txt', $file->filename);
    }

    // -----------------------------------------------------------------------
    // Injectie-tests (#upload_validators)
    // -----------------------------------------------------------------------

    public function testConverterWordtVooraanGehangenEnExtensiesUitgebreid(): void {
        // Realistisch image-field fragment (taal-container + delta met validators).
        $fragment = [
            'und' => [
                0 => [
                    '#upload_validators' => [
                        'file_validate_extensions'       => ['png gif jpg jpeg'],
                        'file_validate_is_image'         => [],
                        'file_validate_image_resolution' => ['7680x7680', '512x512'],
                        'file_validate_size'             => [16777216],
                    ],
                ],
            ],
        ];

        _ozk_profielfoto_heic_validator_inhaken($fragment);

        $validators = $fragment['und'][0]['#upload_validators'];
        $keys       = array_keys($validators);

        $this->assertSame('_ozk_profielfoto_validate_heic_naar_jpg', $keys[0],
            'De converter moet als EERSTE validator draaien.');
        $this->assertStringContainsString('heic', $validators['file_validate_extensions'][0]);
        $this->assertStringContainsString('heif', $validators['file_validate_extensions'][0]);
        // Bestaande validators blijven behouden.
        $this->assertArrayHasKey('file_validate_is_image', $validators);
        $this->assertArrayHasKey('file_validate_image_resolution', $validators);
    }

    public function testInhakenIsIdempotentEnVerdubbeltNiet(): void {
        $fragment = [
            'und' => [
                0 => ['#upload_validators' => ['file_validate_extensions' => ['png gif jpg jpeg']]],
            ],
        ];

        _ozk_profielfoto_heic_validator_inhaken($fragment);
        _ozk_profielfoto_heic_validator_inhaken($fragment); // tweede keer (AJAX-rebuild)

        $exts = $fragment['und'][0]['#upload_validators']['file_validate_extensions'][0];
        $this->assertSame(1, substr_count($exts, 'heic'), 'heic mag maar één keer toegevoegd worden.');
        // Converter staat er ook maar één keer.
        $this->assertSame(1, count(array_filter(
            array_keys($fragment['und'][0]['#upload_validators']),
            fn($k) => $k === '_ozk_profielfoto_validate_heic_naar_jpg'
        )));
    }

    public function testInhakenIsVeiligZonderValidators(): void {
        // Een fragment zonder #upload_validators mag geen fout/wijziging geven.
        $fragment = ['und' => [0 => ['#title' => 'Geen file-element']]];
        $kopie    = $fragment;

        _ozk_profielfoto_heic_validator_inhaken($fragment);

        $this->assertSame($kopie, $fragment, 'Zonder #upload_validators mag er niets veranderen.');
    }
}
