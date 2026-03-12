# relay/main.cpp Security & Stability Hardening — COMPLETED

## All 10 Issues Fixed ✅

| # | Issue | Status | Summary |
|---|-------|--------|---------|
| C1 | Hardcoded secrets → env vars | ✅ | `RELAY_ADMIN_KEY`, `RELAY_WEBHOOK_PATH`, `RELAY_CHARGE_SECRET` env vars. Random admin key fallback with stderr warning. |
| C2 | CHARGE accepts any token | ✅ | Requires `RELAY_CHARGE_SECRET` to be set. Token must be prefixed with the secret. Without secret, CHARGE is disabled. |
| C3 | Session tokens for wallet ops | ✅ | 16-char hex session token issued on JOIN (`SESSION:<token>`). WITHDRAW/TIP/SUPPORT accept optional `:<session_token>` suffix for validation. |
| C4 | Celebrity names in wallets_seed | ✅ | Replaced 52 real names with 16 generic test accounts (DJ-House, DJ-Test-01, etc). Reduced balances to $50-$100. |
| C5 | forward_audio race condition | ✅ | Restructured to hold g_mutex for the entire lookup + destination collection + replay buffer phase. Only sendto() happens outside the lock. Removed unused per-group mutex infrastructure. |
| C6 | volatile bool → atomic | ✅ | `std::atomic<bool> g_running{true}` |
| W1 | Unbounded g_transactions | ✅ | Changed to `std::deque` with 100K cap. Oldest entries evicted. Disk log retains full history. |
| W2 | Replay buffer unconfigurable | ✅ | `--max-replay` CLI flag. Default reduced from 30K to 5K packets. |
| W3 | HTTP server no timeout/rate limit | ✅ | SO_RCVTIMEO (5s), 16KB request cap, per-IP rate limiting (60 req/min), auto-eviction of stale entries. |
| W4 | g_mutex → shared_mutex | ✅ | Upgraded to `std::shared_mutex`. All lock sites updated to `lock_guard<std::shared_mutex>`. Infrastructure ready for selective shared_lock optimization on read paths. |

## Build & Test Results
- Build: Clean (0 warnings, 0 errors)
- Smoke test: JOIN → SESSION token received ✅
- Smoke test: WALLET query ✅
- Smoke test: CHARGE without secret → ERR:charge_disabled ✅
- Smoke test: Admin key validation (wrong key → 403, correct key → 200) ✅
- Smoke test: Webhook endpoint works with env-configured path ✅

## Env Vars Required for Production
```
RELAY_ADMIN_KEY=<strong-random-string>     # Admin API access
RELAY_WEBHOOK_PATH=<stripe-wh-secret>     # Stripe webhook endpoint suffix
RELAY_CHARGE_SECRET=<hmac-secret>         # Required for UDP CHARGE commands
```
