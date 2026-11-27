# Third-party notices

## miniaudio

Scarlett Scout downloads the pinned miniaudio 0.11.26 single-header dependency at build time from:
https://raw.githubusercontent.com/mackron/miniaudio/0.11.26/miniaudio.h

miniaudio is available under the public domain or MIT No Attribution (MIT-0) license. The header's license text is included at the top of the downloaded file.

## OpenVINO Runtime

The optional native Windows NPU worker uses the OpenVINO Runtime C API from Intel's official Windows `vc_mt` archive. The runtime is loaded dynamically from its deployed DLLs; no OpenVINO import library or Python API is used. OpenVINO is distributed under the Apache License 2.0:
https://github.com/openvinotoolkit/openvino/blob/master/LICENSE

Official deployment documentation:
https://docs.openvino.ai/2026/get-started/install-openvino/install-openvino-archive-windows.html

## Music-genre ONNX model

The local model is downloaded from:
https://huggingface.co/onnx-community/Musical-genres-Classification-Hubert-V1-ONNX

The model repository declares Apache-2.0 licensing. The installer verifies the ONNX model SHA-256 before installing it. Genre labels are read from the model repository's `config.json`.

## Windows system libraries

The Win32 application and native NPU worker link against Windows system libraries supplied by Windows and the MinGW-w64 target import libraries. OpenVINO itself is called through its exported C API at runtime.
