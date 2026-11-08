# G1-2 Responses compatibility fixtures — provenance

Status: **official-schema/example-derived synthetic fixture**. None of these
bytes are a captured provider transcript, and no endpoint, credential or
paid API call was involved in producing them.

- Local support contract: `wls-responses-compat-v1` / document subset
  `openai-responses-2026-11-08`; model alias fixture `o3` -> `o3-2025-04-16`
  (official o3 page marks the model Deprecated; used only for offline
  compatibility replay, never as a production model selection).
- Official pages actually read on 2026-11-08 (public docs GET only, cached in
  the private task evidence tree with SHA-256 manifests):
  - `https://developers.openai.com/api/docs/guides/reasoning`
    sha256 `435171ea58b5fe52ae991ddf593482564b351c3baa32cc0549a8e1655d287307`
  - `https://developers.openai.com/api/reference/resources/responses/methods/create`
    sha256 `092db5fa62c9a64d890bbba58e80c12c8a28c1793a29f318cdc5cb0a2bbf1b1a`
  - `https://developers.openai.com/api/reference/resources/responses/streaming-events`
    sha256 `3d54468dae4f176e406a54451087f2901c47d1bde341352694a11383666ad0b4`
  - `https://developers.openai.com/api/docs/guides/streaming-responses`
    sha256 `b6eb6e1bd17cc6c560b1103085439129946b9669ca6a578fae015d74fe1ab971`
  - `https://developers.openai.com/api/docs/models/o3`
    sha256 `f2b10b35cc27286cbee11155c1d0cb1b48262b4600ee0037ece376bd4d692615`
- Official per-event example payloads reused verbatim (field names, shapes,
  and the `rs_6806...` reasoning item ID plus the summary text): the
  `response.reasoning_summary_part.added`, `response.reasoning_summary_text.delta`,
  `response.reasoning_summary_text.done` and `response.reasoning_summary_part.done`
  examples from the streaming-events reference.
- Synthetic assembly disclosed: `sequence_number` values were renumbered into
  one legal contiguous stream, item/response IDs were made unique per fixture,
  and `encrypted_content` values are locally manufactured opaque strings
  (`gAAAAAB...` shape, clearly synthetic) because the official documentation
  does not publish a complete real encrypted transcript. Splitting a summary
  delta into several deltas preserves the official full text exactly.
- Endpoints referenced by profiles that replay these fixtures use
  `.invalid` hosts or the loopback TLS test peer only; nothing here is
  sendable to an official endpoint and no SDK/API call was made.
