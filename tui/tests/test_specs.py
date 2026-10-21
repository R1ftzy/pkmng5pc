"""Host and Vulkan adapter display helpers."""

from __future__ import annotations

import unittest

from tui.specs import (
    GpuSpec,
    format_bytes,
    gpu_label,
    gpu_value,
    parse_gpu_tsv,
    resolve_gpu,
    selected_gpu_text,
)

SAMPLE = "\n".join(
    [
        "# index\tname\ttype\tapi\tmemory_mib\tdriver",
        "0\tArc Graphics\tintegrated\t1.4.325\t9008\t1662916",
        "1\tRTX 4060\tdiscrete\t1.4.341\t7956\t42",
        "not a row",
    ]
)


class GpuParsingTests(unittest.TestCase):
    def test_parses_the_smoke_tool_tsv(self) -> None:
        gpus = parse_gpu_tsv(SAMPLE)
        self.assertEqual(len(gpus), 2)
        self.assertEqual(
            gpus[0], GpuSpec(0, "Arc Graphics", "integrated", "1.4.325", 9008, "1662916")
        )
        self.assertEqual(
            gpus[1], GpuSpec(1, "RTX 4060", "discrete", "1.4.341", 7956, "42")
        )

    def test_values_and_labels_address_the_adapter_by_index(self) -> None:
        gpu = GpuSpec(1, "RTX 4060", "discrete", "1.4.341", 7956, "42")
        self.assertEqual(gpu_value(gpu), "index:1")
        self.assertIn("#1 RTX 4060", gpu_label(gpu))
        self.assertEqual(selected_gpu_text("index:1", [gpu]), gpu_label(gpu))
        self.assertEqual(
            selected_gpu_text("auto", [gpu]), "Auto (prefer discrete)  ·  RTX 4060"
        )

    def test_formats_host_memory(self) -> None:
        self.assertEqual(format_bytes(16 * 1024**3), "16.0 GiB")
        self.assertEqual(format_bytes(512 * 1024**2), "512 MiB")
        self.assertEqual(format_bytes(0), "unknown")


class GpuSelectionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.integrated = GpuSpec(0, "Arc Graphics", "integrated", "1.4.325", 9008, "1")
        self.discrete = GpuSpec(1, "RTX 4060", "discrete", "1.4.341", 7956, "2")
        self.gpus = [self.integrated, self.discrete]

    def test_auto_prefers_discrete(self) -> None:
        self.assertEqual(resolve_gpu("", self.gpus), self.discrete)
        self.assertEqual(resolve_gpu("auto", self.gpus), self.discrete)

    def test_policy_and_explicit_choices_resolve(self) -> None:
        self.assertEqual(resolve_gpu("integrated", self.gpus), self.integrated)
        self.assertEqual(resolve_gpu("index:0", self.gpus), self.integrated)
        self.assertEqual(resolve_gpu("0", self.gpus), self.integrated)
        self.assertEqual(resolve_gpu("4060", self.gpus), self.discrete)

    def test_unknown_choice_falls_back_like_the_presenter(self) -> None:
        self.assertEqual(resolve_gpu("missing", self.gpus), self.discrete)

    def test_selected_text_names_the_resolved_adapter(self) -> None:
        self.assertIn("RTX 4060", selected_gpu_text("discrete", self.gpus))
        self.assertIn("Arc Graphics", selected_gpu_text("integrated", self.gpus))


if __name__ == "__main__":
    unittest.main()
