import gc
import unittest

import numpy as np
from PySide6.QtCore import QCoreApplication, QEvent
from PySide6.QtWidgets import QApplication

from welllog import WellLogView


class WellLogScalarDtypesTest(unittest.TestCase):
    """Every engine ScalarType dtype must submit zero-copy (#36).

    LP64 regression: on Linux/macOS numpy's int64/uint64 report the buffer
    format of C long ('l'/'L', itemsize 8), not the explicit 'q'/'Q'. The
    bridge's scalar_type_for_buffer used to reject them, so submit_curve
    raised WellLogValidationError("unsupported or non-native scalar dtype")
    for plain np.int64/np.uint64 arrays.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.app = QApplication.instance() or QApplication([])

    def test_lp64_int64_formats_are_accepted(self) -> None:
        # Document the regression premise: this host reports C-long formats
        # for int64/uint64 (LP64). On LLP64 Windows these are 'q'/'Q', which
        # were always accepted.
        int64_format = memoryview(np.zeros(2, dtype=np.int64)).format
        uint64_format = memoryview(np.zeros(2, dtype=np.uint64)).format
        self.assertIn(int64_format, ("q", "l"))
        self.assertIn(uint64_format, ("Q", "L"))

    def test_every_scalar_dtype_submits_zero_copy(self) -> None:
        dtypes = (
            np.float32,
            np.float64,
            np.int16,
            np.int32,
            np.int64,
            np.uint8,
            np.uint16,
            np.uint32,
            np.uint64,
        )
        for index, dtype in enumerate(dtypes):
            with self.subTest(dtype=np.dtype(dtype).name):
                view = WellLogView()
                depth = np.arange(1000.0, 1004.0, dtype=np.float64)
                values = np.arange(4, dtype=np.dtype(dtype).type) + 7
                expected_sample = float(values[2])
                depth.flags.writeable = False
                values.flags.writeable = False

                curve_id = f"30000000-0000-4000-8000-0000000000{index:02x}"
                report = view.submit_curve(
                    depth,
                    values,
                    "30000000-0000-4000-8000-0000000000f1",
                    "30000000-0000-4000-8000-0000000000f2",
                    curve_id,
                    "GR",
                    "m",
                    "unit",
                )

                self.assertIs(report["render_prepared"], True)
                self.assertEqual(report["depth"]["access_mode"], "zero_copy")
                self.assertEqual(report["curve"]["access_mode"], "zero_copy")
                self.assertEqual(
                    report["curve"]["dtype"], np.dtype(dtype).name
                )
                self.assertEqual(
                    report["curve"]["address"], values.ctypes.data
                )
                self.assertEqual(
                    report["curve"]["stride_bytes"],
                    np.dtype(dtype).itemsize,
                )
                self.assertEqual(
                    view.sample_value(curve_id, 2), expected_sample
                )

                view.deleteLater()
                QCoreApplication.sendPostedEvents(None, QEvent.DeferredDelete)
                self.app.processEvents()
                gc.collect()


if __name__ == "__main__":
    unittest.main()
