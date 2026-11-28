#!/usr/bin/env python3
# Copyright (c) 2026 Mfweb
# SPDX-License-Identifier: MIT
"""Headless, receive-only KC908 capture to a complex64 file."""

import argparse

from gnuradio import blocks, gr

import kc908


def _nonnegative(value):
    number = int(value)
    if number < 0:
        raise argparse.ArgumentTypeError("must be >= 0")
    return number


def _parser():
    parser = argparse.ArgumentParser(
        description="Capture KC908 IQ samples to a complex64 file without transmitting."
    )
    parser.add_argument("--device-index", type=int, default=0)
    parser.add_argument("--sample-rate", type=int, default=2000000)
    parser.add_argument("--center-freq", type=int, default=100000000)
    parser.add_argument("--port", type=int, choices=(1, 2), default=2)
    parser.add_argument("--attenuation", type=int, default=0)
    parser.add_argument("--if-gain", type=int, default=0)
    parser.add_argument("--amp", type=int, choices=(-1, 0, 1), default=0)
    parser.add_argument("--library-path", default="")
    parser.add_argument("--buffer-samples", type=int, default=16384)
    parser.add_argument(
        "--output", default="kc908_rx.cfile", help="complex64 output file"
    )
    parser.add_argument(
        "--samples",
        type=_nonnegative,
        default=1000000,
        help="finite number of complex samples to capture (0 runs until Ctrl-C)",
    )
    return parser


def main(argv=None):
    args = _parser().parse_args(argv)

    source = kc908.source(
        args.device_index,
        args.sample_rate,
        args.center_freq,
        args.port,
        args.attenuation,
        args.if_gain,
        args.amp,
        args.library_path,
        args.buffer_samples,
    )
    sink = blocks.file_sink(gr.sizeof_gr_complex, args.output)

    top_block = gr.top_block("KC908 RX to file")
    if args.samples:
        head = blocks.head(gr.sizeof_gr_complex, args.samples)
        top_block.connect(source, head, sink)
    else:
        top_block.connect(source, sink)

    top_block.start()
    try:
        top_block.wait()
    except KeyboardInterrupt:
        top_block.stop()
        top_block.wait()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
