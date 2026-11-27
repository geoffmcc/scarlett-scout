The ONNX music-genre model and its config are downloaded and SHA-256 checked by `install.sh`, then deployed to the Windows application folder. The large model and OpenVINO runtime are intentionally not bundled in the source archive.

The current model is a ten-class GTZAN-style broad genre classifier. It is not a dedicated instrument, break, loop, or subgenre model. Track-level scores are aggregated across successive excerpts and withheld as mixed/uncertain until there are enough windows and repeated winning votes. Scores are model-relative, not calibrated probabilities.
