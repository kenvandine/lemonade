# Lemonade Omni Models

**Lemonade Omni Models** provide true all-to-all omni-modality to users and apps. They accomplish this by unifying the capabilities of a collection of an LLM, an image model, an ASR model, and a TTS model — everything a multimodal agent needs to chat, generate images, transcribe audio, and speak responses out loud.

Under the hood, Lemonade Omni Models are powered by **OmniRouter** — Lemonade's pattern for exposing each modality as an OpenAI-compatible tool that an existing LLM agent can call against Lemonade's endpoints.

You bring the LLM loop. Lemonade brings the local tools.

## How OmniRouter works

1. Describe the tools to your LLM in OpenAI tool-calling format.
2. The LLM decides which tool to call and with what arguments.
3. Your client executes each `tool_call` against the corresponding Lemonade endpoint, such as `/v1/images/generations` or `/v1/audio/speech`.
4. The client sends the tool result back to the LLM as a `tool` message.
5. The LLM continues until it either calls another tool or returns a final response.

The tool schemas OmniRouter provides are plain JSON. They do not require a Lemonade-specific client library, and the endpoints they target use OpenAI-compatible request and response shapes.

## The omni models

An **omni model** is a virtual model made up of components, registered with `recipe: "collection.omni"`. Lemonade ships these:

| Omni model | LLM | Image | ASR | TTS |
|-----------|-----|-------|-----|-----|
| **LMX-Omni-52B-Halo** | Qwen3.6-35B-A3B-MTP-GGUF | Flux-2-Klein-9B-GGUF (gen + edit) | Whisper-Large-v3-Turbo | kokoro-v1 |
| **LMX-Omni-5.5B-Lite** | Qwen3.5-4B-MTP-GGUF | SD-Turbo (gen only) | Whisper-Tiny | kokoro-v1 |

Omni models are hidden from the default `/v1/models` listing so OpenAI-compatible clients don't see "LMX-Omni-52B-Halo" as if it were an LLM. They surface with `?show_all=true` and appear in the Lemonade desktop app's Model Manager under the **Lemonade** category.

### Naming scheme

Omni model names follow the pattern `LMX-Omni-<xB>-<class>`:

| Component | Value | Meaning |
|-----------|-------|---------|
| Org prefix | `LMX` | Lemonade Mix. |
| Modality | `Omni` | True all-to-all omni-modal bundle. |
| Size | `xB` | Total parameter count across all component models. |
| Class | `Halo` | Based on a large MoE LLM (e.g., targeted at Strix Halo). |
|  | `Lite` | Based on small models targeted at 32 GB APUs. |
|  | `Dense` | Based on a dense LLM targeted at 32 GB dGPUs (none shipped yet). |

### Use an omni model

Every part of this doc assumes one is loaded — the desktop app, [`examples/lemonade_tools.py`](https://github.com/lemonade-sdk/lemonade/blob/main/examples/lemonade_tools.py), and the tools themselves were all validated against the two omni models above.

If you're the developer wiring OmniRouter into your own agent and you want to substitute models, you can, but you take on the compatibility work: any LLM you swap in must carry the `tool-calling` label, and each tool you want to call needs one downloaded model whose `labels` include the row's "Needs a model with label" entry from the tools table below. That's a developer-path discovery step, not a user configuration; the simple answer for everyone else is "install an omni model."

## Custom Omni Models

You can build your own omni model from registered models — see [Register a custom Omni Model from the desktop app](../guide/configuration/custom-models.md#register-a-custom-omni-model-from-the-desktop-app) in the custom models guide.

## Available tools

The canonical definitions live in [`src/app/src/renderer/utils/toolDefinitions.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/app/src/renderer/utils/toolDefinitions.json) — a single source of truth used by the desktop app and this documentation.

| Tool | Endpoint | Needs a model with label |
|------|----------|--------------------------|
| `generate_image` | `POST /v1/images/generations` | `image` |
| `edit_image` | `POST /v1/images/edits` | `edit` |
| `text_to_speech` | `POST /v1/audio/speech` | `tts` |
| `transcribe_audio` | `POST /v1/audio/transcriptions` | `transcription` |
| `analyze_image` | `POST /v1/chat/completions` | LLM with `vision` |

Endpoint request/response shapes are documented in the [Endpoints Spec](../api/README.md).

## Quick start

```bash
pip install openai
python examples/lemonade_tools.py "Generate an image of a sunset"
python examples/lemonade_tools.py "Say hello world out loud"
```

[`examples/lemonade_tools.py`](https://github.com/lemonade-sdk/lemonade/blob/main/examples/lemonade_tools.py) shows the full agentic loop — tool definitions, LLM call with `tools=[...]`, executing each `tool_call`, and feeding the result back. Fewer than 150 lines of Python.

## Using your own agent

Integrate OmniRouter into an existing agent by following the pattern in [`examples/lemonade_tools.py`](https://github.com/lemonade-sdk/lemonade/blob/main/examples/lemonade_tools.py):

1. Point your OpenAI-compatible client at `http://localhost:13305/v1`.
2. Copy the tool entries from [`src/app/src/renderer/utils/toolDefinitions.json`](https://github.com/lemonade-sdk/lemonade/blob/main/src/app/src/renderer/utils/toolDefinitions.json) into your agent's tool list (or load the JSON directly).
3. When your agent receives a `tool_call` for one of these tools, POST to the corresponding endpoint from the table above and feed the response back to the LLM as a `tool` message.
4. If you want to pick models programmatically rather than rely on an omni model being loaded, query `GET /v1/models?show_all=true` and match the `labels` array against the "Needs a model with label" column above.

The example script implements all four steps end-to-end against the `generate_image` and `text_to_speech` tools.

## Managing collections via the API

Omni collections are registered and managed through the same REST endpoints as regular models.

### Register a collection

```bash
curl -X POST http://localhost:13305/v1/pull \
  -H "Content-Type: application/json" \
  -d '{
        "model_name": "user.MyOmniKit",
        "recipe": "collection.omni",
        "components": ["Qwen3-0.6B-GGUF", "SD-Turbo", "Whisper-Tiny", "kokoro-v1"]
      }'
```

All components must already be registered (built-in models, or previously pulled `user.*` models). Components that are registered but not yet downloaded are pulled automatically as part of this call.

### Query collections

Collections are hidden from the default `/v1/models` listing so they don't appear as plain LLMs to OpenAI-compatible clients. Use `?show_all=true` to include them:

```bash
curl "http://localhost:13305/v1/models?show_all=true"
```

You can identify a collection in the response by checking `recipe == "collection.omni"` in each model object. The `labels` array on the collection entry reflects the union of its components' labels.

To discover which models are suitable for a given tool role, filter `GET /v1/models?show_all=true` by label:

```python
import requests

models = requests.get("http://localhost:13305/v1/models?show_all=true").json()["data"]

image_models     = [m for m in models if "image"         in m.get("labels", [])]
tts_models       = [m for m in models if "tts"           in m.get("labels", [])]
asr_models       = [m for m in models if "transcription" in m.get("labels", [])]
vision_models    = [m for m in models if "vision"        in m.get("labels", [])]
```

This lets you build a dynamic model picker rather than hardcoding a specific omni model name.

### Delete a collection

Deleting a collection removes only the collection registry entry. Component models remain on disk and can still be used independently.

```bash
curl -X POST http://localhost:13305/v1/delete \
  -H "Content-Type: application/json" \
  -d '{"model_name": "user.MyOmniKit"}'
```

To also free disk space, delete each component individually after deleting the collection.

## Component loading behavior

When you load a collection (`POST /v1/load` or the first inference request), Lemonade loads all components eagerly — the LLM, image model, ASR model, and TTS model are all started before the first request returns. This ensures tool calls can be dispatched immediately once the collection is ready, at the cost of higher startup VRAM.

**LRU eviction and collections:** Each component occupies its own LRU slot within its model type (one LLM slot, one image slot, one ASR slot, one TTS slot). If another model of the same type is loaded while a component is in its slot, that component will be evicted. After eviction, the next tool call targeting that component will re-load it automatically before the request continues — adding latency. To avoid this, set `max_loaded_models` high enough to hold all components you intend to use concurrently.

**Collections do not hold model-type slots** — only their individual components do. Deleting the collection entry does not evict its components from memory.

## Chat-transcription models

`chat-transcription` models (e.g. Qwen2.5-Omni) are a different integration path from OmniRouter. These are LLMs that accept audio directly in the `/v1/chat/completions` message payload — you do not need tool calls or a collection. The model processes audio and text in a single forward pass.

### When to use each approach

| | OmniRouter (collection) | Chat-transcription model |
|---|---|---|
| **Mechanism** | Tool calls dispatched to separate specialized models | Single model accepts mixed audio+text |
| **Models needed** | LLM + ASR + image + TTS (separate) | One multimodal model |
| **VRAM** | Higher (multiple models loaded) | Lower (one model) |
| **Flexibility** | Mix and match any compatible models | Depends on what the single model supports |
| **Use when** | You want best-in-class per modality or hardware that fits separate models | You want the simplest integration and have a suitable multimodal model |

### Sending audio in chat completions

For models labeled `chat-transcription`, include an audio attachment in the `content` array of a user message using the `input_audio` content type:

```bash
curl -X POST http://localhost:13305/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
        "model": "Qwen2.5-Omni-7B",
        "messages": [
          {
            "role": "user",
            "content": [
              {"type": "text", "text": "What is being said in this audio?"},
              {
                "type": "input_audio",
                "input_audio": {
                  "data": "<base64-encoded audio bytes>",
                  "format": "wav"
                }
              }
            ]
          }
        ]
      }'
```

To identify `chat-transcription` models in your app:

```python
models = requests.get("http://localhost:13305/v1/models").json()["data"]
chat_asr_models = [m for m in models if "chat-transcription" in m.get("labels", [])]
```
