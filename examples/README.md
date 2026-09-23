# SoapySidekiq examples

The Python examples in [`python`](python/) demonstrate common SoapySDR
workflows with a Sidekiq device. They require a configured Sidekiq system and
the SoapySDR Python bindings.

Start with [`tx_tone.py`](python/tx_tone.py) when creating a new transmit
application. It demonstrates device selection, radio configuration, stream
setup, MTU-sized writes, error handling, and reliable stream cleanup.

## Examples

- `tx_tone.py` continuously transmits a generated CS16 or CF32 tone.
- `tx_tone_1pps.py` starts tone transmission on an external PPS edge.
- `rx_to_file.py` captures a finite number of MTU-sized blocks.
- `record_rx_to_file.py` records continuously or for a specified duration.
- `tx_from_file.py` transmits interleaved little-endian CS16 samples from a
  file once or repeatedly.

Run any example with `--help` to see its complete command-line interface. For
example:

```sh
python3 examples/python/tx_tone.py --help
python3 examples/python/rx_to_file.py capture.bin --blocks 100
python3 examples/python/record_rx_to_file.py capture.bin --duration 10
python3 examples/python/tx_from_file.py capture.bin
```

## Sample representation

The file examples use CS16 samples. One stream element is one complex sample
containing a signed 16-bit I value followed by a signed 16-bit Q value. Each
complex sample therefore occupies four bytes.

Every stream operation requests at most the stream MTU. Receive examples write
only the number of samples actually returned by `readStream()`. This is
important because a successful SoapySDR stream operation may return fewer
elements than requested.

The examples configure sample rate, bandwidth, frequency, and gain before
calling `setupStream()`. Streams are always deactivated and closed, including
when an exception or Ctrl-C stops the program.
