"""Exercise the actual C DSP without microphone access or speaker output."""

import ctypes as c
import itertools
import math
import pathlib
import random
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class Metrics(c.Structure):
    _fields_ = (
        [
            (name, c.c_float)
            for name in (
                "input_peak",
                "output_peak",
                "limiter_reduction_db",
                "safety_reduction_db",
                "feedback_hz",
                "buffer_ms",
            )
        ]
        + [
            (name, c.c_uint32)
            for name in (
                "notch_count",
                "underruns",
                "overruns",
                "fault_reason",
            )
        ]
        + [("tripped", c.c_bool)]
    )


def tone(hz, amplitude=0.2, seconds=1, rate=48000):
    return [
        amplitude * math.sin(2 * math.pi * hz * i / rate)
        for i in range(int(rate * seconds))
    ]


def rms(values):
    return math.sqrt(sum(x * x for x in values) / len(values))


class ProcessorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        destination = ROOT / ".build" / "tests"
        destination.mkdir(parents=True, exist_ok=True)
        library = destination / "libSonoDSP.dylib"
        subprocess.run(
            [
                "clang",
                "-std=c11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-unused-parameter",
                "-dynamiclib",
                "-I",
                str(ROOT / "Sources/SonoDSP/include"),
                str(ROOT / "Sources/SonoDSP/Processor.c"),
                str(ROOT / "Sources/SonoDSP/Hardware.c"),
                "-framework",
                "CoreAudio",
                "-framework",
                "AudioToolbox",
                "-framework",
                "CoreFoundation",
                "-o",
                str(library),
            ],
            check=True,
        )
        cls.lib = c.CDLL(str(library))
        cls.lib.sono_create.argtypes = [c.c_double, c.c_double, c.c_uint32]
        cls.lib.sono_create.restype = c.c_void_p
        cls.lib.sono_destroy.argtypes = [c.c_void_p]
        cls.lib.sono_set_gain.argtypes = [c.c_void_p, c.c_float]
        for name in ("sono_set_muted", "sono_set_feedback_enabled"):
            getattr(cls.lib, name).argtypes = [c.c_void_p, c.c_bool]
        cls.lib.sono_process.argtypes = [
            c.c_void_p,
            c.POINTER(c.c_float),
            c.POINTER(c.c_float),
            c.c_uint32,
        ]
        cls.lib.sono_push.argtypes = [c.c_void_p, c.POINTER(c.c_float), c.c_uint32]
        cls.lib.sono_render.argtypes = [c.c_void_p, c.POINTER(c.c_float), c.c_uint32]
        cls.lib.sono_metrics.argtypes = [c.c_void_p]
        cls.lib.sono_metrics.restype = Metrics
        cls.lib.sono_audio_open.argtypes = [
            c.c_uint32,
            c.c_uint32,
            c.c_uint32,
            c.c_bool,
            c.c_char_p,
            c.c_size_t,
        ]
        cls.lib.sono_audio_open.restype = c.c_void_p

    def create(self, input_rate=48000, output_rate=48000):
        p = self.lib.sono_create(input_rate, output_rate, 512)
        self.assertTrue(p)
        self.addCleanup(self.lib.sono_destroy, p)
        return p

    def process(self, p, samples):
        array = c.c_float * len(samples)
        output = array()
        self.lib.sono_process(p, array(*samples), output, len(samples))
        return list(output)

    def open(self, p):
        self.lib.sono_set_muted(p, False)
        self.lib.sono_set_feedback_enabled(p, False)
        self.lib.sono_set_gain(p, 0)

    def test_starts_muted_and_emergency_mute_is_silent(self):
        p = self.create()
        self.assertTrue(all(x == 0 for x in self.process(p, tone(700))))
        self.open(p)
        self.assertGreater(rms(self.process(p, tone(700))[-10000:]), 0.01)
        self.lib.sono_set_muted(p, True)
        self.assertTrue(all(x == 0 for x in self.process(p, tone(700))))

    def test_limiter_and_nonfinite_input(self):
        p = self.create()
        self.open(p)
        self.lib.sono_set_gain(p, 12)
        output = self.process(p, tone(700, 8) + [math.nan, math.inf, -math.inf])
        self.assertTrue(all(math.isfinite(x) and abs(x) <= 0.501 for x in output))
        self.assertGreater(self.lib.sono_metrics(p).limiter_reduction_db, 3)

    def test_highpass_attenuates_rumble(self):
        low, high = self.create(), self.create()
        self.open(low)
        self.open(high)
        self.assertLess(
            rms(self.process(low, tone(20))[-20000:]),
            rms(self.process(high, tone(1000))[-20000:]) * 0.1,
        )

    def test_sustained_tone_triggers_notch_and_gain_reduction(self):
        p = self.create()
        self.lib.sono_set_gain(p, 0)
        self.lib.sono_set_muted(p, False)
        output = self.process(p, tone(1500, 0.3, 1.5))
        m = self.lib.sono_metrics(p)
        self.assertGreater(m.notch_count, 0)
        self.assertAlmostEqual(m.feedback_hz, 1500, delta=70)
        self.assertGreaterEqual(m.safety_reduction_db, 5)
        self.assertLess(rms(output[-10000:]), rms(output[2000:6000]) * 0.3)

    def test_broadband_noise_does_not_trigger_feedback(self):
        p = self.create()
        self.lib.sono_set_muted(p, False)
        source = random.Random(7)
        self.process(p, [source.uniform(-0.05, 0.05) for _ in range(96000)])
        self.assertEqual(self.lib.sono_metrics(p).notch_count, 0)
        self.assertFalse(self.lib.sono_metrics(p).tripped)

    def test_repeated_resonances_latch_cutoff(self):
        p = self.create()
        self.lib.sono_set_gain(p, 0)
        self.lib.sono_set_muted(p, False)
        for hz in (1000, 2000, 3000, 4000):
            self.process(p, tone(hz, 0.4))
        self.assertTrue(self.lib.sono_metrics(p).tripped)
        self.lib.sono_set_muted(p, False)
        self.assertTrue(all(x == 0 for x in self.process(p, tone(800))))

    def test_ring_resamples_and_underrun_is_silent(self):
        p = self.create(44100, 48000)
        self.open(p)
        output = (c.c_float * 1600)()
        self.lib.sono_render(p, output, 256)
        self.assertTrue(all(x == 0 for x in output[:256]))
        samples = tone(1000, seconds=0.04, rate=44100)
        self.lib.sono_push(p, (c.c_float * len(samples))(*samples), len(samples))
        self.lib.sono_render(p, output, 1600)
        self.assertGreater(rms(output[256:1500]), 0.005)
        crossings = sum(a <= 0 < b for a, b in itertools.pairwise(output[256:1500]))
        self.assertAlmostEqual(crossings, 26, delta=2)

    def test_overflow_trips_without_overwriting_audio(self):
        p = self.create()
        samples = (c.c_float * 40000)()
        self.lib.sono_push(p, samples, 40000)
        self.assertTrue(self.lib.sono_metrics(p).tripped)
        self.assertEqual(self.lib.sono_metrics(p).fault_reason, 2)

    def test_invalid_rates_rejected(self):
        self.assertFalse(self.lib.sono_create(0, 48000, 512))
        self.assertFalse(self.lib.sono_create(48000, math.nan, 512))

    def test_invalid_devices_fail_without_starting_audio(self):
        error = c.create_string_buffer(512)
        self.assertFalse(self.lib.sono_audio_open(0, 0, 0, False, error, len(error)))
        self.assertIn(b"Core Audio", error.value)

    def test_downsampling_attenuates_out_of_band_signal(self):
        levels = []
        for hz in (1000, 40000):
            p = self.create(96000, 48000)
            self.open(p)
            source = tone(hz, seconds=0.01, rate=96000)
            chunk = (c.c_float * len(source))(*source)
            output = (c.c_float * 480)()
            for _ in range(100):
                self.lib.sono_push(p, chunk, len(source))
                self.lib.sono_render(p, output, 480)
            self.assertFalse(self.lib.sono_metrics(p).tripped)
            levels.append(rms(output))
        self.assertLess(levels[1], levels[0] * 0.05)

    def test_clock_drift_in_both_directions_remains_stable(self):
        for drift in (-0.0002, 0.0002):
            p = self.create()
            self.open(p)
            output = (c.c_float * 480)()
            source = (c.c_float * 481)(*tone(1000, seconds=481 / 48000))
            total = 0
            for tick in range(6000):
                next_total = int((tick + 1) * 480 * (1 + drift))
                self.lib.sono_push(p, source, next_total - total)
                self.lib.sono_render(p, output, 480)
                total = next_total
            metrics = self.lib.sono_metrics(p)
            self.assertFalse(metrics.tripped)
            self.assertEqual(metrics.underruns, 0)
            self.assertEqual(metrics.overruns, 0)

    def test_continuous_stream_remains_stable_for_one_minute(self):
        for input_rate in (44100, 48000):
            with self.subTest(input_rate=input_rate):
                p = self.create(input_rate, 48000)
                self.open(p)
                output = (c.c_float * 480)()
                chunk = tone(1000, seconds=0.01, rate=input_rate)
                source = (c.c_float * len(chunk))(*chunk)
                for _ in range(6000):
                    self.lib.sono_push(p, source, len(chunk))
                    self.lib.sono_render(p, output, 480)
                metrics = self.lib.sono_metrics(p)
                self.assertFalse(metrics.tripped)
                self.assertEqual(metrics.overruns, 0)
                self.assertEqual(metrics.underruns, 0)
                self.assertGreater(rms(output), 0.005)

    def test_short_tone_does_not_trigger_and_music_mode_preserves_note(self):
        p = self.create()
        self.lib.sono_set_muted(p, False)
        self.process(p, tone(1500, seconds=0.1) + [0] * 24000)
        self.assertEqual(self.lib.sono_metrics(p).notch_count, 0)
        self.lib.sono_set_feedback_enabled(p, False)
        self.process(p, tone(1500, seconds=2))
        self.assertEqual(self.lib.sono_metrics(p).notch_count, 0)


if __name__ == "__main__":
    unittest.main()
