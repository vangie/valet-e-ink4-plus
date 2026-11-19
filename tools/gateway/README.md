# Shared Valet gateway

Adapters, pairing lifecycle and protocol compatibility are maintained in the independent repository:

https://github.com/vangie/valet-gateway

This firmware pins `integrations/valet-gateway` as a submodule. Initialize it with `git submodule update --init --recursive`. Install the adapter on the gateway host using that pinned revision's documentation. Plugin IDs remain `ai-passport-openclaw` / `ai-passport-hermes`; moving source does not require existing Passport devices to re-pair.

`enable_valet.py` is a compatibility wrapper for an older deployed adapter. Its implementation resides in the shared repository and adds the E-Ink4 signed identity while retaining Passport identity. Normal plugin reloads may interrupt active clients; do not apply a deployed upgrade in the middle of a conversation. The isolated interop tests do not replace or restart deployed gateways.

Ask currently uploads a 16 kHz mono mu-law WAV, with a text response and history fallback. Live negotiates Passport v1, uploads 24 kHz IMA frames, decodes server IMA into PCM playback, and supports manual interruption and mute. Physical audio, echo handling and 4G throughput still require device acceptance. Book contexts, book Live and owned answer speech are negotiated optional features in v0.2.0.

Pinned release: [Valet Gateway v0.2.0](https://github.com/vangie/valet-gateway/releases/tag/v0.2.0), commit `d935cf3c90c51fd783d9d327cbfc86457771c83f`. Adapter package versions remain 0.8.0. Verify downloads against the release `SHA256SUMS.txt` and `release-manifest.json`. This pin update does not deploy or restart gateways.
