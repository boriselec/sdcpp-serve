# sdcpp-serve reference

> [!NOTE]
> Written by AI from the code and test measurements.

sdcpp-serve runs image generation models on the CPU. It is a Docker image with
[stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp)'s `sd-server` and
a small router in front of it. Every model gets its own sd-server process, and all of
them are reached through one port, 8081, under `/<name>/`.

| File | Contents |
| --- | --- |
| `server.cpp` | the router |
| `Dockerfile` | builds stable-diffusion.cpp and the router, ships both on a distroless base |
| `compose.example.yaml` | a complete setup, with model downloads |
| `.github/workflows/docker-publish.yml` | publishes the image to `ghcr.io/boriselec/sdcpp-serve` |

## Models are container arguments

Each container argument is one model, written as `name=<sd-server flags>`. The name is
the URL prefix. The flags go to sd-server as they are, split on whitespace. There is no
quoting, so a flag value cannot contain spaces.

The router wraps the flags like this:

```
sd-server --mmap <flags> --listen-ip 127.0.0.1 --listen-port <1234 + n>
```

sd-server uses the last copy of a repeated flag, so a model cannot move sd-server off
localhost or onto another port. `--mmap` cannot be turned off.

Paths in the flags are paths inside the container, normally under `/models`. Anything
not set falls back to sd-server's defaults: 512×512, 20 steps, CFG 7, seed 42, and flash
attention, which sd-server switches on for CPUs by itself. The fixed seed means the same
prompt gives the same image.

There is no config file and no built-in model. Without arguments the server exits with a
usage message.

## Running

### Compose

`compose.example.yaml` is the intended setup. Copy it to `compose.yaml`, which git
ignores, adjust it, and run `docker compose up -d`.

The `models` service downloads model files into the named volume `sdcpp-serve-models`.
The `sdcpp-serve` service starts once the download has finished and reads the same
volume read-only. It publishes port 8081 on `127.0.0.1` only. Opening it to the network
is a deliberate edit of the `ports` line.

### Plain Docker

Without compose there are no downloads. Put the files into a folder and mount it:

```sh
docker run -d --restart unless-stopped -p 127.0.0.1:8081:8081 \
  -v "$PWD/models:/models:ro" \
  ghcr.io/boriselec/sdcpp-serve:latest \
  'flux=--diffusion-model /models/flux-2-klein-4b-Q4_0.gguf --llm /models/Qwen3-4B-Q4_K_M.gguf
    --vae /models/full_encoder_small_decoder.safetensors --cfg-scale 1.0 --steps 4'
```

## Model files

### Downloads

The `models` service is curl with `-fL --fail-early -C - --remote-name-all` and a list
of URLs. Every file is saved under the last part of its URL.

- A missing file is downloaded, a partial one is resumed, and a complete one is left
  alone. curl makes one short request per file on every start to find out which case
  applies.
- The volume is mounted at curl's home folder, `/home/curl_user`, which is also its
  working directory. A new named volume takes the owner of the path it is mounted on, so
  curl writes as its normal user. The service runs without root and without any
  capabilities.
- The server waits for the download with `required: false`. When curl fails, because
  there is no internet or Hugging Face is down, compose prints a warning and starts the
  server with the files that are already there.

Two consequences follow from `-C -`. curl opens every existing file for writing, so
files placed by hand must belong to uid 100, otherwise curl exits with 23 on every start
(the server still starts). And a download that was interrupted leaves a partial file
under its real name. If the next start has no internet, sd-server fails to load it and
the container restarts until a start with internet completes the file.

To place files by hand, write them into
`/var/lib/docker/volumes/sdcpp-serve-models/_data/`. `docker compose down -v` deletes
the volume and the models with it.

### The example models

The URLs point at fixed Hugging Face commits, so the files cannot change. Their sha256
was checked against downloaded copies.

| File | Repository | Size | sha256 | License |
| --- | --- | --- | --- | --- |
| `flux-2-klein-4b-Q4_0.gguf` | `leejet/FLUX.2-klein-4B-GGUF` | 2.46 GB | `d1023499ef3f2f82ff7c50e6778495195c1b6cc34835741778868428111f9ff4` | Apache 2.0 |
| `Qwen3-4B-Q4_K_M.gguf` | `unsloth/Qwen3-4B-GGUF` | 2.50 GB | `f6f851777709861056efcdad3af01da38b31223a3ba26e61a4f8bf3a2195813a` | Apache 2.0 |
| `full_encoder_small_decoder.safetensors` | `black-forest-labs/FLUX.2-small-decoder` | 0.25 GB | `ea4273f02d1fafbf8e1d1c2cf6018ed8748652eb0bf34f2dd91171f16f15ab62` | Apache 2.0 |
| `dreamlike-diffusion-1.0.safetensors` | `dreamlike-art/dreamlike-diffusion-1.0` | 2.13 GB | `c86a1a99b8c2a2891d9fac4990a8b90cee598d3e16c682fae76c0ab31212983d` | modified CreativeML OpenRAIL-M |

FLUX.2 klein 4B is the default. It is a distilled model and needs `--cfg-scale 1.0` and
`--steps 4`; with sd-server's defaults it would take about ten times as long and give
worse results. The small decoder is used instead of the standard FLUX.2 VAE because it
is ungated and lighter on a CPU. Dreamlike Diffusion 1.0 is a Stable Diffusion 1.5
fine-tune and is commented out in the example. Its license forbids use on sites or apps
that earn revenue or donations.

## API

| Request | Handled by |
| --- | --- |
| `POST /<name>/generate` | the router itself |
| any other `GET` or `POST` under `/<name>/` | passed on to that model's sd-server |
| anything else | `404` |

`POST /<name>/generate` takes the prompt as plain text in the body and answers with a
PNG (`image/png`). It calls sd-server's `/v1/images/generations` and decodes the image
from the JSON reply.

| `/generate` status | When |
| --- | --- |
| 200 | the PNG |
| 404 | unknown model |
| 502 | sd-server gave no answer |
| 503 | another POST is running |
| 500 | the prompt is not valid UTF-8, or sd-server's reply could not be read |
| others | sd-server's own error, with its status and message |

Everything else is sd-server's own API: the OpenAI-style `/v1/images/generations` and
`/v1/images/edits`, the AUTOMATIC1111-style `/sdapi/v1/txt2img` and `/img2img`, the
async `/sdcpp/v1/` jobs, `/v1/models` and more, see
[its API docs](https://github.com/leejet/stable-diffusion.cpp/blob/master/examples/server/api.md).

One POST runs at a time across all models, `/generate` and sd-server's alike, because a
generation takes all CPUs. Another POST gets `503` right away instead of waiting. GETs,
such as progress and job status, are not limited. The async `/sdcpp/v1/` jobs answer at
once and generate in the background, so the lock does not cover their generation.

The router forwards the method, the path with its query string, the body and the
`Content-Type`. It answers with sd-server's status, body and `Content-Type`; other
headers are dropped. Multipart uploads such as `/v1/images/edits` are rebuilt from their
fields and files, because cpp-httplib parses them on arrival. The router waits up to an
hour for sd-server.

sd-server reads per-request settings from any prompt, written as
`<sd_cpp_extra_args>{"seed": 1, "width": 768, "height": 768}</sd_cpp_extra_args>`. The
block is removed before generation. This also works through `/generate`.

## Process

The router runs as PID 1 and starts the sd-servers as child processes. All of them run
as the distroless `nonroot` user, uid 65532.

- When any sd-server exits, the router exits with code 1, and the restart policy brings
  the whole container back.
- SIGTERM and SIGINT end it at once. The kernel then stops the sd-servers.
- The router listens immediately and does not wait for the sd-servers. They load their
  models lazily and answer within a second; earlier requests get `502`.
- An argument without `=`, or a name used twice, stops the router with a message.

## Memory and speed

Weights are memory-mapped (`--mmap`). They live in the kernel's page cache and are read
from disk again when memory runs short, so an idle model costs little memory of its own.
Measured at 512×512:

| Model | Weights | Working memory while generating | Idle |
| --- | --- | --- | --- |
| FLUX.2 klein 4B | 4.8 GB | up to 1.5 GB | 0.23 GB |
| Dreamlike (SD 1.5) | 2.0 GB | up to 1.7 GB | 0.07 GB |

The peaks are in the final VAE decode. Both models fit into 8 GB of RAM as long as one
image is generated at a time; switching models then re-reads part of the weights from
disk. `--vae-tiling` lowers the SD 1.5 peak to 0.5 GB and costs about half a minute per
image on four cores.

On 8 x86 cores without SMT, FLUX.2 klein takes about 34 seconds per step with all 8
threads, about 2.5 minutes per image, measured with flash attention for the diffusion
model only. sd-server uses one thread per physical core, which is all 8 there. The
pinned release miscounts them on Linux and uses half the CPUs, which took twice as long;
the image patches that. The count covers the whole machine and does not see a
`docker --cpus` or `--cpuset-cpus` limit; set `--threads` in the flags in that case.

## Image and build

The Dockerfile has four stages.

1. **source** fetches stable-diffusion.cpp on Ubuntu 24.04. The commit is pinned by hash
   in `SDCPP_COMMIT`, with the release name as a comment (`master-975-95b3090`,
   2026-10-18). A `sed` fixes the `/sys` path sd.cpp counts physical cores from.
2. **build** compiles it the same way upstream's `docker/Dockerfile` does, without the
   web UI. On amd64 it builds all 14 x86 CPU variants as separate libraries, and ggml
   loads the best one for the CPU at startup. On arm64 it builds for the CPU of the
   machine doing the build, because the all-variants build fails there with GCC 13.
3. **server** compiles `server.cpp` with cpp-httplib and nlohmann/json, both taken from
   stable-diffusion.cpp's `thirdparty/` folder, in parallel with **build**.
4. The final image is `gcr.io/distroless/cc-debian13:nonroot`. It has glibc, libstdc++
   and libgomp, and no shell, package manager or root user. Its glibc must not be older
   than the build stage's.

| Path | Contents |
| --- | --- |
| `/opt/sd.cpp/` | sd-server, sd-cli and their libraries |
| `/usr/local/bin/sdcpp-serve` | the router, the entrypoint |
| `/usr/share/doc/sdcpp-serve/` | `LICENSE`, and the license files of stable-diffusion.cpp and the libraries it builds in |

The arm64 image is about 28 MB to download and 116 MB on disk. The amd64 image is larger
by the CPU variants, about 17 MB unpacked.

Every push to `master` publishes `:latest`, built for amd64 only.

Building locally works with `docker build .`, but compiling stable-diffusion.cpp needs a
lot of memory. One of its files embeds about 140 MB of tokenizer data and alone needs
3.4 GB to compile, and the build runs one compile job per CPU. Four jobs ran a 4 GB
machine out of memory; eight jobs on an 8 GB arm64 machine peaked at 6.2 GB.

## Decisions

**CPU only.** The image is for machines without a GPU. GPU users are better served by
upstream's CUDA and Vulkan images.

**stable-diffusion.cpp instead of PyTorch.** The image is a few dozen MB instead of
gigabytes, and a 4-step model like FLUX.2 klein is faster on a CPU than SD 1.5 at 50
steps was with diffusers.

**One sd-server per model.** Every model is always loaded and needs no switching. Memory
mapping keeps the cost of idle models low. Upstream's own model switching appeared after
the pinned release and is not used.

**A small C++ router, no Python.** It builds from libraries already in
stable-diffusion.cpp's source and lets the image run on a distroless base. It does only
routing, `/generate` and process supervision.

**Models are container arguments.** sd-server flags are the format users already know
from upstream's docs. Compose holds the whole setup in one readable file.

**Downloads live outside the image.** A stock curl container fetches files before the
server starts. The server never touches the network. Offline machines just leave the
files in place.

**Everything is pinned.** stable-diffusion.cpp by commit hash, model files by Hugging
Face commit. A tag or branch could change underneath a build. There is no hash check of
the downloads; the pinned commits do that job.

**Build like upstream, run on distroless.** The build stage matches upstream's tested
setup. The runtime has only what the binaries need. Distroless supports its `cc` images
longer than its Python ones.

**One generation at a time, `503` instead of waiting.** Two generations would compete
for the same cores and memory. Callers know immediately and can retry, and no requests
pile up in the router. The lock covers every POST, because any of sd-server's can
generate.

**Locked down by default.** The port is published on localhost only. Both containers run
as non-root users, with read-only file systems, no capabilities and no privilege
escalation.

**sd-server's defaults wherever possible.** The example sets only what a model needs.
The router adds `--mmap`, and nothing else.

## Updating

To move to a newer stable-diffusion.cpp, pick a release tag (`master-<n>-<hash>`), put
its full commit hash into `SDCPP_COMMIT` and update the comment. If the build stage
moves to a newer Ubuntu, check that the runtime's glibc is still at least as new;
otherwise the container fails at once with `version 'GLIBC_2.xx' not found`. Releases
after `master-975` add model loading and unloading under `/sdcpp/v1/models/`, which the
router would pass on; consider blocking those paths when updating. Once upstream fixes
the `/sys` path, the `sed` in the source stage does nothing and can go.

To change a model file, take the commit from the repository's history on Hugging Face
and use it in the `resolve/<commit>/` URL.

## License

The project is under [AGPL-3.0-only](../LICENSE). The image also contains
stable-diffusion.cpp and ggml (MIT) and the libraries built into them, with their
license files in `/usr/share/doc/sdcpp-serve/third-party/`. Models are under their own
licenses, listed above.
