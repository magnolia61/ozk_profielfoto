#!/usr/bin/env python3
"""
Unit-tests voor de PURE beslislogica van bin/ozk-facedetect:
  - _iou()                 — overlap tussen twee boxes
  - _kies_beste()          — size-filter + consensus + winnaarkeuze (stuurt ook de
                             productie-cascade aan: None ⇒ CNN-vangnet vuurt)
  - _schaal_voor_detectie()— verkleining naar DETECT_MAX (detectie ≠ crop-resolutie)

Geen echte foto's of zware libs nodig voor de kernlogica: we voeren synthetische
detector-resultaten in. De script-detectoren (HOG/YuNet/CNN) doen hun zware imports
lazy bínnen de functie, dus het importeren van het script is licht en veilig.

Draaien:
    /home/webteam/venv/facedetect/bin/python3 -m unittest discover \
        -s sites/all/modules/ozk_profielfoto/tests/python -v
"""

import importlib.machinery
import importlib.util
import os
import unittest

# Het script heet 'ozk-facedetect' (koppelteken, geen .py-extensie) → expliciet
# via een SourceFileLoader laden (spec_from_file_location kan zonder extensie geen
# loader afleiden).
_SCRIPT = os.path.join(os.path.dirname(__file__), "..", "..", "bin", "ozk-facedetect")


def _load_module():
    loader = importlib.machinery.SourceFileLoader("ozk_facedetect", _SCRIPT)
    spec = importlib.util.spec_from_loader("ozk_facedetect", loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


fd = _load_module()


def box(top, right, bottom, left, score=1.0, naam="x"):
    """Bouw een detector-box in het interne formaat (top, right, bottom, left, score, naam)."""
    return (top, right, bottom, left, score, naam)


def res(**per_detector):
    """Bouw een resultaten-dict: res(hog=[box(...)], yunet=[box(...)])."""
    return {naam: {"ms": 1, "boxes": boxes} for naam, boxes in per_detector.items()}


class IouTest(unittest.TestCase):

    def test_identieke_box_is_1(self):
        b = box(0, 100, 100, 0)
        self.assertAlmostEqual(fd._iou(b, b), 1.0, places=6)

    def test_geen_overlap_is_0(self):
        a = box(0, 100, 100, 0)
        b = box(0, 300, 100, 200)  # rechts ernaast, raakt niet
        self.assertEqual(fd._iou(a, b), 0.0)

    def test_halve_overlap(self):
        # a = 0..100 breed/hoog (opp 10000). b = 50..150 (opp 10000).
        # intersectie = 50x50 = 2500; unie = 10000+10000-2500 = 17500.
        a = box(0, 100, 100, 0)
        b = box(50, 150, 150, 50)
        self.assertAlmostEqual(fd._iou(a, b), 2500 / 17500, places=6)


class KiesBesteTest(unittest.TestCase):

    def test_te_klein_wordt_gefilterd_en_geeft_none(self):
        # Loa-scenario: enige detectie is een mini-box (false positive in de stof).
        # min_px = 240 ⇒ box van 107px valt eronder ⇒ None (productie laat dan
        # de foto onaangeroerd, en de cascade roept CNN als vangnet aan).
        resultaten = res(yunet=[box(2242, 2922, 2349, 2815, 0.6, "yunet")])  # 107x107
        beste, plausibel = fd._kies_beste(resultaten, min_px=240)
        self.assertIsNone(beste)
        self.assertEqual(plausibel, [])

    def test_geen_gezicht_geeft_none(self):
        resultaten = res(hog=[], yunet=[])
        beste, _ = fd._kies_beste(resultaten, min_px=100)
        self.assertIsNone(beste)

    def test_consensus_box_wint_van_losse(self):
        # hog+yunet vinden (overlappend) hetzelfde gezicht → support 1 elk.
        # mediapipe vindt een losse (niet-overlappende) box → support 0.
        # Winnaar moet uit het bevestigde paar komen, niet de losse.
        gezicht_hog   = box(100, 400, 400, 100, 1.0, "hog")    # 300x300 @ (100,100)
        gezicht_yunet = box(110, 410, 410, 110, 0.9, "yunet")  # overlapt hog
        los_mediapipe = box(100, 900, 400, 700, 0.9, "mediapipe")  # ver weg, groter? nee
        resultaten = res(hog=[gezicht_hog], yunet=[gezicht_yunet], mediapipe=[los_mediapipe])
        (beste, support), _ = fd._kies_beste(resultaten, min_px=100)
        self.assertIn(beste[5], ("hog", "yunet"))
        self.assertGreaterEqual(support, 1)

    def test_grootste_wint_bij_gelijke_consensus(self):
        # Twee bevestigde clusters; beide support ≥1. De grootste box wint.
        # cluster A (klein): hog+yunet ~200px. cluster B (groot): cnn+extra ~400px.
        a1 = box(0, 200, 200, 0, 1.0, "hog")
        a2 = box(5, 205, 205, 5, 0.9, "yunet")
        b1 = box(0, 400, 400, 0, 1.0, "cnn")
        b2 = box(5, 405, 405, 5, 0.9, "yunet")  # tweede yunet-box (cluster B)
        resultaten = {
            "hog":   {"ms": 1, "boxes": [a1]},
            "cnn":   {"ms": 1, "boxes": [b1]},
            "yunet": {"ms": 1, "boxes": [a2, b2]},
        }
        (beste, _support), _ = fd._kies_beste(resultaten, min_px=100)
        # Grootste cluster (B, 400px) moet winnen.
        self.assertGreaterEqual(beste[1] - beste[3], 400)

    def test_multiface_consensus_kan_kleinere_kiezen(self):
        # Documenteert het waargenomen multi-face-gedrag: volwassene (groot, 1 detector)
        # vs baby (kleiner, door 2 detectoren bevestigd) → de bevestigde baby wint,
        # ook al is de volwassene groter. (Reden om later een 'grootste+centraal'-regel
        # te overwegen.)
        volwassene = box(100, 700, 700, 100, 1.0, "hog")        # 600x600, support 0
        baby_mp    = box(800, 1100, 1100, 800, 0.7, "mediapipe")  # 300x300
        baby_yu    = box(810, 1110, 1110, 810, 0.6, "yunet")      # overlapt baby_mp
        resultaten = res(hog=[volwassene], mediapipe=[baby_mp], yunet=[baby_yu])
        (beste, support), _ = fd._kies_beste(resultaten, min_px=100)
        self.assertIn(beste[5], ("mediapipe", "yunet"))
        self.assertEqual(support, 1)


class SchaalTest(unittest.TestCase):

    def setUp(self):
        try:
            import numpy  # noqa: F401
        except ImportError:
            self.skipTest("numpy niet beschikbaar")

    def test_grote_foto_wordt_verkleind(self):
        import numpy as np
        img = np.zeros((3200, 2400, 3), dtype=np.uint8)  # langste zijde 3200
        klein, factor = fd._schaal_voor_detectie(img)
        self.assertAlmostEqual(factor, fd.DETECT_MAX / 3200.0, places=6)
        self.assertEqual(max(klein.shape[0], klein.shape[1]), fd.DETECT_MAX)

    def test_kleine_foto_blijft_ongewijzigd(self):
        import numpy as np
        img = np.zeros((800, 600, 3), dtype=np.uint8)  # kleiner dan DETECT_MAX
        klein, factor = fd._schaal_voor_detectie(img)
        self.assertEqual(factor, 1.0)
        self.assertEqual(klein.shape, img.shape)


class OrientatieTest(unittest.TestCase):
    """Oriëntatie-zoeker. De échte rotatie-herkenning op een gedraaide-zonder-EXIF
    foto is een end-to-end gegeven (shell-test op portret90.jpg → suggested_rotation
    270 → upright crop); hier toetsen we de pure randvoorwaarde: vindt HOG in geen
    enkele stand een gezicht, dan komt er GEEN (verkeerde) rotatie uit."""

    def setUp(self):
        try:
            import numpy  # noqa: F401
            import cv2     # noqa: F401
            import face_recognition  # noqa: F401
        except ImportError:
            self.skipTest("numpy/cv2/face_recognition niet beschikbaar")

    def test_geen_gezicht_geeft_geen_rotatie(self):
        import numpy as np
        # Egale ruis zonder gezicht → HOG vindt in geen enkele stand iets → 0.
        img = np.zeros((600, 400, 3), dtype=np.uint8)
        self.assertEqual(fd._zoek_orientatie_hog(img, min_px=50), 0)


if __name__ == "__main__":
    unittest.main()
