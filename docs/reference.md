# sdcpp-serve reference

> [!NOTE]
> Written by AI from the code and test measurements.

sdcpp-serve is a Docker image that runs image generation models on the CPU: one
[stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) `sd-server` per
model, behind a small router on port 8081. A prompt goes in as plain text, a PNG comes
out.

| File | Contents |
| --- | --- |
| `server.cpp` | the router |
| `Dockerfile` | builds stable-diffusion.cpp and the router, ships both on distroless |
| `compose.example.yaml` | a complete setup, with model downloads |
| `compile_flags.txt` | the router's compiler flags, for clangd |
| `.github/workflows/docker-publish.yml` | publishes `ghcr.io/boriselec/sdcpp-serve:latest` |

## Decisions

**CPU only.** GPU users are better served by upstream's CUDA and Vulkan images.

**stable-diffusion.cpp, not PyTorch.** The image is a few dozen MB, not gigabytes.

**One sd-server per model, all loaded all the time.** No model switching, upstream's
included. Memory mapping keeps idle models cheap.

**Models and their settings are fixed at startup.** A model's flags are all its
settings. No request changes the model, size, steps or seed, so the same prompt gives
the same image.

**One endpoint: `POST /<name>/generate`.** Everything else gets `404`, sd-server's own
API included, and so does anything new releases add.

**Synchronous, one image at a time.** A request during a generation gets `503` at once
instead of waiting in a queue.

**The body is the prompt, as UTF-8 text.** Anything else, a JSON request, a multipart
form, binary, or an oversized upload, is refused, not read as a prompt.

**A `4xx` is final, a `5xx` is worth retrying.** A `4xx` is about the request and comes
back the same every time. A `5xx` is about the server and the same request can succeed
later. This holds for sd-server's own errors too, passed on unchanged.

**A small C++ router, no Python.** It builds from libraries already in
stable-diffusion.cpp's source and runs on distroless.

**Models are container arguments.** They are written in sd-server's own flags. Compose
holds the whole setup in one file; there is no config file.

**Downloads live outside the image.** A stock curl container fetches the model files.
The server never touches the network.

**Everything is pinned.** stable-diffusion.cpp by commit, model files by Hugging Face
commit. There is no hash check; the commits do that job.

**Build like upstream, run on distroless.**

**Locked down by default.** The port is published on localhost only. Both containers run
as non-root users, with read-only file systems, no capabilities and no privilege
escalation.

**sd-server's defaults wherever possible.** The router adds `--mmap` and nothing else.

## Models

Each container argument is a model, `name=<sd-server flags>`, served at
`/<name>/generate`. The flags are split on whitespace, with no quoting. The router runs

```
sd-server --mmap <flags> --listen-ip 127.0.0.1 --listen-port <1234 + n>
```

sd-server takes the last copy of a repeated flag, so a model cannot leave localhost.
`--mmap` cannot be turned off.

Paths are inside the container, normally under `/models`. What the flags leave unset is
sd-server's default: 512×512, 20 steps, CFG 7, seed 42, flash attention on for CPUs.

Without arguments, with an argument without `=`, or with a name used twice, the router
exits with a message.

## Running

### Compose

Copy `compose.example.yaml` to `compose.yaml`, which git ignores, adjust it, and run
`docker compose up -d`. The `models` service downloads into the volume
`sdcpp-serve-models`. `sdcpp-serve` starts after it and mounts the volume read-only at
`/models`.

### Docker

Without compose there are no downloads. Mount a folder with the files:

```sh
docker run -d --restart unless-stopped -p 127.0.0.1:8081:8081 \
  -v "$PWD/models:/models:ro" \
  ghcr.io/boriselec/sdcpp-serve:latest \
  'flux=--diffusion-model /models/flux-2-klein-4b-Q4_0.gguf --llm /models/Qwen3-4B-Q4_K_M.gguf
    --vae /models/full_encoder_small_decoder.safetensors --cfg-scale 1.0 --steps 4'
```

## Model files

The `models` service is curl with `-fL --fail-early -C - --remote-name-all` and a list
of URLs. It runs as curl's own user in its home folder, where the volume is mounted.

- Missing files are downloaded, partial ones resumed, complete ones left alone. That
  costs one short request per file on every start.
- Without internet curl fails, and the server starts with the files already there
  (`required: false`).
- Files placed by hand go into `/var/lib/docker/volumes/sdcpp-serve-models/_data/` and
  must belong to uid 100, otherwise curl exits with 23 on every start.
- An interrupted download leaves a partial file under its real name. sd-server fails to
  load it, and the container restarts until a start with internet completes it.
- `docker compose down -v` deletes the volume with the models.

### The example models

| File | Repository | Size | sha256 | License |
| --- | --- | --- | --- | --- |
| `flux-2-klein-4b-Q4_0.gguf` | `leejet/FLUX.2-klein-4B-GGUF` | 2.46 GB | `d1023499ef3f2f82ff7c50e6778495195c1b6cc34835741778868428111f9ff4` | Apache 2.0 |
| `Qwen3-4B-Q4_K_M.gguf` | `unsloth/Qwen3-4B-GGUF` | 2.50 GB | `f6f851777709861056efcdad3af01da38b31223a3ba26e61a4f8bf3a2195813a` | Apache 2.0 |
| `full_encoder_small_decoder.safetensors` | `black-forest-labs/FLUX.2-small-decoder` | 0.25 GB | `ea4273f02d1fafbf8e1d1c2cf6018ed8748652eb0bf34f2dd91171f16f15ab62` | Apache 2.0 |
| `dreamlike-diffusion-1.0.safetensors` | `dreamlike-art/dreamlike-diffusion-1.0` | 2.13 GB | `c86a1a99b8c2a2891d9fac4990a8b90cee598d3e16c682fae76c0ab31212983d` | modified CreativeML OpenRAIL-M |

FLUX.2 klein 4B is distilled and needs `--cfg-scale 1.0 --steps 4`. The small decoder
replaces the standard FLUX.2 VAE: it is ungated and lighter on a CPU. Dreamlike
Diffusion 1.0 is an SD 1.5 fine-tune, commented out in the example. Its license forbids
sites and apps that earn revenue or donations.

## API

`POST /<name>/generate` with the prompt as the body. The router sends only the prompt to
sd-server's `/v1/images/generations`, waits up to an hour, and answers with the PNG from
`data[0].b64_json`.

The body must be UTF-8 text: valid UTF-8 without control characters other than tab and
line breaks. Its `Content-Type` must be `text/plain`,
`application/x-www-form-urlencoded` (curl's default) or absent; a `charset` is ignored.
It must not be a JSON object with a string `"prompt"`, which is an OpenAI-style request,
nor contain `<sd_cpp_extra_args>`, sd-server's per-request settings. Other JSON is a
prompt like any text, such as the structured prompts of FLUX.2 and Fibo.

| Status | When |
| --- | --- |
| 200 | the PNG, `image/png` |
| 400 | the body is not UTF-8 text, a JSON request, or has `<sd_cpp_extra_args>`; the reason is a plain-text body |
| 404 | an unknown model, or any other method or path |
| 413 | the body is over 8 KiB |
| 415 | an unsupported `Content-Type` |
| 500 | an unexpected error, such as a reply from sd-server that cannot be read |
| 502 | no answer from sd-server, also while it is starting |
| 503 | another image is being generated, by any model |

An sd-server error is passed on with its status and body: a `4xx` for a refused prompt
or a model that generates no image, a `5xx` for a generation that fails. The router logs
each one, and its own `500`, as `sdcpp-serve: <path>: ...`.

## Process

The router is PID 1 and starts the sd-servers as its children, all as the distroless
`nonroot` user, uid 65532.

- It listens at once. The sd-servers load their models lazily and answer within a
  second.
- When an sd-server exits, the router exits with 1, and the restart policy restarts the
  container.
- SIGTERM and SIGINT end it at once. The kernel then stops the sd-servers.

## Memory and speed

Weights are memory-mapped and live in the page cache. An idle model costs little memory
of its own; the kernel reads weights from disk again when memory runs short. At 512×512:

| Model | Weights | Working memory while generating | Idle |
| --- | --- | --- | --- |
| FLUX.2 klein 4B | 4.8 GB | up to 1.5 GB | 0.23 GB |
| Dreamlike (SD 1.5) | 2.0 GB | up to 1.7 GB | 0.07 GB |

The peaks are in the final VAE decode. Both models fit into 8 GB, one image at a time.
`--vae-tiling` lowers the SD 1.5 peak to 0.5 GB and adds about half a minute per image
on four cores.

| CPU | FLUX.2 klein 4B, 4 steps | Dreamlike, 50 steps |
| --- | --- | --- |
| 8 x86 cores, no SMT | 2.5 min | – |
| 8 arm64 cores | 2 min | 7 min |

sd-server runs one thread per physical core of the machine and does not see a
`docker --cpus` or `--cpuset-cpus` limit. Set `--threads` in the flags then.

## Image and build

1. **source**: stable-diffusion.cpp at `SDCPP_COMMIT`, release `master-995-fd21ecf`
   (2026-10-22), on Ubuntu 24.04.
2. **build**: upstream's CPU build, without the web UI. On amd64 all 14 x86 CPU
   variants, ggml loads the best one at startup. On arm64 native to the build machine;
   the all-variants build fails there with GCC 13.
3. **server**: the router, C++23, with cpp-httplib, nlohmann/json and utf8proc from
   stable-diffusion.cpp's `thirdparty/`, in parallel with **build**.
4. **final**: `gcr.io/distroless/cc-debian13:nonroot`, with glibc, libstdc++ and libgomp
   and no shell, package manager or root. Its glibc must not be older than the build
   stage's.

| Path | Contents |
| --- | --- |
| `/opt/sd.cpp/` | sd-server, sd-cli and their libraries |
| `/usr/local/bin/sdcpp-serve` | the router, the entrypoint |
| `/usr/share/doc/sdcpp-serve/` | `LICENSE`, and the license files of stable-diffusion.cpp and what it builds in |

The arm64 image is about 29 MB to download and 117 MB on disk. amd64 adds about 17 MB of
CPU variants. Every push to `master` publishes `:latest`, for amd64 only.

`docker build .` needs memory: it runs one compile job per CPU, and one file, with about
140 MB of tokenizer data, alone needs 3.4 GB. Eight jobs peaked at 6.2 GB on an 8 GB
arm64 machine; four jobs ran a 4 GB machine out of memory.

## Updating

- **stable-diffusion.cpp**: put a release's full commit hash into `SDCPP_COMMIT` and its
  name, `master-<n>-<hash>`, into the comment. Then check upstream's
  `examples/server/routes_openai.cpp` for what the router relies on:
  `/v1/images/generations` takes `{"prompt": ...}` and replies with `data[0].b64_json`,
  and `extract_and_remove_sd_cpp_extra_args()` still looks for `<sd_cpp_extra_args>`.
  New endpoints need nothing.
- **Ubuntu**: a newer build stage needs a runtime glibc at least as new, otherwise the
  container fails at once with `version 'GLIBC_2.xx' not found`.
- **Model files**: take the commit from the file's history on Hugging Face and put it
  into the `resolve/<commit>/` URL.

## License

[AGPL-3.0-only](../LICENSE). The image also contains stable-diffusion.cpp and ggml (MIT)
and the libraries built into them, with their license files in
`/usr/share/doc/sdcpp-serve/third-party/`. Models are under their own licenses, see the
example models.
