# Native request-path benchmark

Use a sealed Linux ARM64 package and a new output directory outside the checkout. The driver starts the native gateway and a scripted loopback upstream in a fresh network namespace, with a private temporary directory and empty home. The stream trial checks progressive delivery, and inference trials verify quota settlement, dispatch counts and terminal delivery.

```sh
bash cpp/gateway/bench/request_path/isolated.sh --package "$sealed_package" --out "$scratch/results"
python3 -B cpp/gateway/bench/request_path/summarize.py "$scratch/results/results.json"
```

The receipt records package and executable hashes, latency, throughput, CPU cost and peak RSS. Use `--scenarios`, `--repetitions`, `--requests` and `--warmup` to select a smaller run. Use `--profile <scenario>` for one instrumented trial.
