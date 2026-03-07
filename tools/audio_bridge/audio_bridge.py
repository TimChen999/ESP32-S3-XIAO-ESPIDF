#!/usr/bin/env python3
"""
Audio Bridge — relays audio between a Wokwi-simulated ESP32 and the host PC.

The ESP32 firmware (mic_driver.c, speaker_driver.c) calls these endpoints
when compiled with MIC_SIMULATE=1 / SPEAKER_SIMULATE=1:

  GET  /mic      — records a chunk from the PC mic, returns raw PCM bytes
  POST /speaker  — receives raw PCM bytes, plays them on the PC speakers
  GET  /health   — returns OK (connectivity check)

Start this server BEFORE launching the Wokwi simulation.

Usage:
  pip install -r requirements.txt
  python audio_bridge.py [--port 8080] [--sample-rate 16000] [--channels 1]

Test without ESP32:
  curl http://localhost:8080/health
  curl http://localhost:8080/mic -o chunk.raw
  curl -X POST http://localhost:8080/speaker --data-binary @chunk.raw
"""

import argparse
import sys

import numpy as np
import sounddevice as sd
from flask import Flask, request, Response

app = Flask(__name__)

# Persistent audio streams — initialized once in main(), used by endpoints.
# Keeping streams open avoids per-request open/close overhead from PortAudio.
mic_stream = None
speaker_stream = None
chunk_samples = 512


# ── ESP32 mic_driver.c entry point ──────────────────────────────────────────
# mic_capture_task() calls GET /mic each iteration of its capture loop
# (when MIC_SIMULATE=1). The response body is raw PCM that fills the
# capture_buf, replacing what would normally come from I2S RX hardware.
@app.route('/mic', methods=['GET'])
def get_mic():
    """Record a chunk from the PC microphone and return raw PCM bytes.
    Blocks until the chunk is captured, providing natural real-time pacing
    back to the ESP32 capture task."""
    data, overflowed = mic_stream.read(chunk_samples)
    if overflowed:
        app.logger.warning("Mic input overflowed — audio may have gaps")
    return Response(bytes(data), mimetype='application/octet-stream')


# ── ESP32 speaker_driver.c entry point ──────────────────────────────────────
# speaker_playback_task() calls POST /speaker each iteration of its playback
# loop (when SPEAKER_SIMULATE=1). The request body is raw PCM that would
# normally be written to I2S TX hardware.
@app.route('/speaker', methods=['POST'])
def play_speaker():
    """Receive raw PCM bytes and play them on the PC speakers.
    The blocking write to the output stream provides natural back-pressure
    pacing back to the ESP32 playback task."""
    pcm_data = request.data
    if len(pcm_data) > 0:
        audio = np.frombuffer(pcm_data, dtype=np.int16)
        speaker_stream.write(audio)
    return Response('OK', status=200, mimetype='text/plain')


@app.route('/health', methods=['GET'])
def health():
    """Connectivity check — call from ESP32 or curl to verify the bridge
    is running and reachable."""
    return Response('OK', status=200, mimetype='text/plain')


def main():
    global mic_stream, speaker_stream, chunk_samples

    parser = argparse.ArgumentParser(
        description='Audio bridge between Wokwi ESP32 simulation and host PC audio',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Audio format must match the ESP32 driver configuration (AUDIO_PRESET in
mic_driver.c / speaker_driver.c). The default is speech mode:
  16000 Hz, 1 channel, 16-bit signed PCM

Examples:
  python audio_bridge.py                        # defaults
  python audio_bridge.py --port 9090            # custom port
  python audio_bridge.py --list-devices         # show available audio devices
  python audio_bridge.py --input-device 2       # use specific mic

Test with curl:
  curl http://localhost:8080/health
  curl http://localhost:8080/mic -o chunk.raw
  curl -X POST http://localhost:8080/speaker --data-binary @chunk.raw
""")
    parser.add_argument('--host', default='0.0.0.0',
                        help='Bind address (default: 0.0.0.0)')
    parser.add_argument('--port', type=int, default=8080,
                        help='Listen port (default: 8080)')
    parser.add_argument('--sample-rate', type=int, default=16000,
                        help='Sample rate in Hz (default: 16000, must match ESP32)')
    parser.add_argument('--channels', type=int, default=1,
                        help='Channel count (default: 1, must match ESP32)')
    parser.add_argument('--chunk-samples', type=int, default=512,
                        help='Samples per mic chunk (default: 512 = 1024 bytes)')
    parser.add_argument('--list-devices', action='store_true',
                        help='List available audio devices and exit')
    parser.add_argument('--input-device', type=int, default=None,
                        help='Input device index (use --list-devices to see)')
    parser.add_argument('--output-device', type=int, default=None,
                        help='Output device index (use --list-devices to see)')
    args = parser.parse_args()

    if args.list_devices:
        print(sd.query_devices())
        sys.exit(0)

    chunk_samples = args.chunk_samples

    try:
        mic_stream = sd.RawInputStream(
            samplerate=args.sample_rate,
            channels=args.channels,
            dtype='int16',
            device=args.input_device,
        )
        speaker_stream = sd.RawOutputStream(
            samplerate=args.sample_rate,
            channels=args.channels,
            dtype='int16',
            device=args.output_device,
        )
        mic_stream.start()
        speaker_stream.start()
    except Exception as e:
        print(f"Failed to open audio devices: {e}", file=sys.stderr)
        print("Run with --list-devices to see available devices.", file=sys.stderr)
        sys.exit(1)

    chunk_bytes = args.chunk_samples * 2 * args.channels
    chunk_ms = args.chunk_samples * 1000 / args.sample_rate

    print(f"Audio Bridge for ESP32 Wokwi Simulation")
    print(f"  Server:   http://{args.host}:{args.port}")
    print(f"  Format:   {args.sample_rate} Hz, {args.channels}ch, 16-bit signed")
    print(f"  Chunk:    {args.chunk_samples} samples ({chunk_bytes} bytes, {chunk_ms:.0f}ms)")
    print(f"  Endpoints:")
    print(f"    GET  /mic      — captures from PC microphone")
    print(f"    POST /speaker  — plays on PC speakers")
    print(f"    GET  /health   — connectivity check")
    print()

    app.run(host=args.host, port=args.port, threaded=True)


if __name__ == '__main__':
    main()
