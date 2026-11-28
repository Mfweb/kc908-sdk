# Copyright (c) 2026 Mfweb
# SPDX-License-Identifier: MIT
"""Generate both GRC blocks, route variable callbacks, and run offline graphs."""

import os
from pathlib import Path
import runpy
import sys
from unittest.mock import patch

from gnuradio import gr
from gnuradio.grc.core.platform import Platform


def main():
    library = os.environ.get("KTOS_SDK_LIBRARY")
    emulator = "FTD3XX.dll" if os.name == "nt" else "libusb-1.0.so.0"
    if not library or not (Path(library).parent / emulator).is_file():
        raise RuntimeError("Run with CTest's test-only SDK/emulator")
    output = Path(sys.argv[1])
    output.mkdir(parents=True, exist_ok=True)
    version = tuple(int(part) for part in gr.version().split(".")[:3])
    platform = Platform(version=gr.version(), version_parts=version)
    platform.build_library(
        [
            str(Path(gr.prefix()) / "share/gnuradio/grc/blocks"),
            str(Path(__file__).parents[1] / "grc"),
        ]
    )
    for kind in ("source", "sink"):
        graph = platform.make_flow_graph()
        name = "kc908_grc_" + kind
        graph.options_block.params["id"].set_value(name)
        # These blocks generate Python; do not inherit GRC's output language default.
        graph.options_block.params["output_language"].set_value("python")
        graph.options_block.params["generate_options"].set_value("no_gui")
        graph.grc_file_path = str(output / (name + ".grc"))

        def add(block_id, label):
            block = graph.new_block(block_id)
            block.params["id"].set_value(label)
            return block

        hardware = add("kc908_" + kind, "hardware")
        for key, value in (("samp_rate", "2000000"), ("center_freq", "100000000")):
            variable = add("variable", key)
            variable.params["value"].set_value(value)
        hardware.params["sample_rate"].set_value("samp_rate")
        hardware.params["center_freq"].set_value("center_freq")
        if kind == "source":
            head = add("blocks_head", "head")
            discard = add("blocks_null_sink", "discard")
            graph.rewrite()
            graph.connect(hardware.sources[0], head.sinks[0])
            graph.connect(head.sources[0], discard.sinks[0])
        else:
            samples = add("blocks_vector_source_x", "samples")
            samples.params["repeat"].set_value("False")
            graph.rewrite()
            graph.connect(samples.sources[0], hardware.sinks[0])
        graph.rewrite()
        graph.validate()
        assert graph.is_valid(), list(graph.get_error_messages())
        assert hardware.flags.throttle, "GRC must recognize the hardware clock"
        hardware.params["port"].set_value("0")
        graph.rewrite()
        graph.validate()
        assert not graph.is_valid(), "GRC accepted an invalid physical port"
        hardware.params["port"].set_value("2" if kind == "source" else "1")
        graph.rewrite()
        graph.validate()
        assert graph.is_valid(), list(graph.get_error_messages())

        generator = platform.Generator(graph, str(output))
        generator.write()
        module = runpy.run_path(str(output / (name + ".py")))
        for mode in ("0", "1"):
            with patch.dict(os.environ, {"KTOS_TEST_MODE": mode, "KTOS_TEST_FAILURE": ""}):
                tb = module[name]()
                try:
                    with patch.object(
                        tb.hardware, "set_center_freq", wraps=tb.hardware.set_center_freq
                    ) as setter:
                        tb.set_center_freq(144640000)
                        setter.assert_called_once_with(144640000)
                    with patch.object(
                        tb.hardware, "set_sample_rate", wraps=tb.hardware.set_sample_rate
                    ) as setter:
                        tb.set_samp_rate(2500000)
                        setter.assert_called_once_with(2500000)
                    tb.run()
                finally:
                    tb.stop()
                    tb.wait()
        print(f"{kind}: GRC validation, callbacks, original/KTOS generated flowgraphs passed")


if __name__ == "__main__":
    main()
