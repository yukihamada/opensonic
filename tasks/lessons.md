# Lessons Learned

- **Default channels = 1 (Mono)**: All platforms MUST default to mono. User has repeatedly emphasized this. Locations: iOS SettingsView `@AppStorage("channels")`, Mac SettingsView, web `baChannels`, `ba-output-ch` select order, `resetToDefaults()`. Bridge header already says "default: 1".
- **solunad lock file**: Only one instance can run at a time. `pkill -9 solunad` to force stop before re-testing.
- **Relay deploy**: `cd apps/relay && fly deploy -a soluna-relay`. Web deploy: `cp web/* deploy/web/ && cd deploy && fly deploy -a soluna-web`.
- **Stripe webhook**: Created `we_1TAY4dDqLakc8NxkykO9NW85`, URL uses RELAY_WEBHOOK_PATH secret suffix. STRIPE_WEBHOOK_SECRET set for signature verification.
- **Security: device_id vs device_name**: Financial operations (wallets, royalty) must use `device_id` (UUID), not `device_name` (user-provided, spoofable). Channel ownership checks should match both for backward compat.
