<?php

use PHPUnit\Framework\TestCase;

/**
 * Tests voor de crop-geometrie van de vergelijkmodus: _ozk_vgl_geom() uit
 * ozk_profielfoto.vergelijk.inc. Dezelfde formule die de productie-crop gebruikt
 * (2.5× padding, 42% centrering, klemmen binnen de afbeelding) — dus dit dekt de
 * kernberekening van de uitsnede zonder ImageMagick aan te roepen.
 *
 * Draaien:
 *   cd sites/all/modules/ozk_profielfoto
 *   phpunit9 --configuration=phpunit.xml.dist
 */
class VergelijkGeomTest extends TestCase {

    public static function setUpBeforeClass(): void {
        // De .inc declareert alleen functies; Drupal/CiviCRM-aanroepen zitten in de
        // mail-functie (runtime), niet op include-niveau → veilig te laden met de stubs.
        require_once __DIR__ . '/../../ozk_profielfoto.vergelijk.inc';
    }

    /** Helper: box in het formaat dat _ozk_vgl_geom verwacht. */
    private function box(int $top, int $right, int $bottom, int $left): array {
        return ['top' => $top, 'right' => $right, 'bottom' => $bottom, 'left' => $left];
    }

    public function testBasisCrop25En42(): void {
        // 100×100 gezicht gecentreerd op (500,500) in 1000×1000.
        $g = _ozk_vgl_geom($this->box(450, 550, 550, 450), 2.5, 0.42, 1000, 1000);
        $this->assertSame(250, $g['zijde']);          // 100 * 2.5
        $this->assertSame(375, $g['left']);           // 500 - 125
        $this->assertSame(395, $g['top']);            // 500 - round(250*0.42)=105
    }

    public function testPaddingSchaaltDeZijde(): void {
        $box = $this->box(450, 550, 550, 450);
        $this->assertSame(200, _ozk_vgl_geom($box, 2.0, 0.42, 1000, 1000)['zijde']);
        $this->assertSame(300, _ozk_vgl_geom($box, 3.0, 0.42, 1000, 1000)['zijde']);
    }

    public function testCentreringVerschuiftVerticaal(): void {
        $box   = $this->box(450, 550, 550, 450);
        $c42   = _ozk_vgl_geom($box, 2.5, 0.42, 1000, 1000)['top'];
        $c50   = _ozk_vgl_geom($box, 2.5, 0.50, 1000, 1000)['top'];
        // Hogere centerwaarde → crop begint hoger (kleinere top) → gezicht zakt in beeld.
        $this->assertLessThan($c42, $c50);
        $this->assertSame(375, $c50);                 // 500 - 125
    }

    public function testMinimaleZijdeVan100(): void {
        // Piepklein 10×10 gezicht → 10*2.5=25, maar minimaal 100.
        $g = _ozk_vgl_geom($this->box(495, 505, 505, 495), 2.5, 0.42, 1000, 1000);
        $this->assertSame(100, $g['zijde']);
    }

    public function testZijdeGekaptOpBeeldgrootte(): void {
        // 100×100 gezicht, padding 2.5 = 250, maar beeld is maar 200×200.
        $g = _ozk_vgl_geom($this->box(50, 150, 150, 50), 2.5, 0.42, 200, 200);
        $this->assertSame(200, $g['zijde']);          // min(250, 200, 200)
        $this->assertSame(0, $g['left']);             // geklemd
        $this->assertSame(0, $g['top']);              // geklemd
        $this->assertGreaterThanOrEqual(0, $g['left']);
    }

    public function testKlemtBinnenRechterrand(): void {
        // Gezicht tegen de rechterrand → left mag niet buiten W-zijde vallen.
        $g = _ozk_vgl_geom($this->box(450, 1000, 550, 900), 2.5, 0.42, 1000, 1000);
        $this->assertSame(750, $g['left']);           // W(1000) - zijde(250)
        $this->assertLessThanOrEqual(1000 - $g['zijde'], $g['left']);
    }

    public function testCropBlijftBinnenGrenzen(): void {
        // Eigenschapscheck: voor diverse posities valt de crop altijd binnen [0..W/H].
        foreach ([[100, 100], [900, 100], [500, 950], [50, 50]] as [$cx, $cy]) {
            $box = $this->box($cy - 50, $cx + 50, $cy + 50, $cx - 50);
            $g = _ozk_vgl_geom($box, 2.5, 0.42, 1000, 1000);
            $this->assertGreaterThanOrEqual(0, $g['left']);
            $this->assertGreaterThanOrEqual(0, $g['top']);
            $this->assertLessThanOrEqual(1000, $g['left'] + $g['zijde']);
            $this->assertLessThanOrEqual(1000, $g['top'] + $g['zijde']);
        }
    }
}
