# Trials HD leaderboard server

A tiny web service that stores the times the recompiled game uploads and serves them back in
place of Xbox Live leaderboards. Python standard library only, one SQLite file.

**Easiest: run it free on Cloudflare Workers instead**. Nothing to host at home; see
[worker/README.md](worker/README.md). The rest of this page is the self-hosted (Docker) version.

## Run it on Unraid (no image build needed)

1. Copy `trials_server.py` to `/mnt/user/appdata/trialshd/` (e.g. through the `appdata` share).
2. **Docker → Add Container** (turn on *Advanced View*):
   - Name: `trialshd-server`
   - Repository: `python:3.12-slim`
   - Post Arguments: `python3 /data/trials_server.py`
   - Add **Path**: container `/data` → host `/mnt/user/appdata/trialshd`
   - Add **Port**: container `8080` → host `8095` (any free port)
   - Add **Variable**: `TRIALS_ADMIN_KEY` = a long random string (optional, for admin endpoints)
   - Optional **Variable**: `TRIALS_EVERYONE_FRIENDS` = `0` if you don't want every player on the
     server to be everyone's friend (see Friends below)
3. Start it and open `http://<unraid-ip>:8095/health`; it should say `{"ok": true}`.

## Put it on the internet with Cloudflare Tunnel

1. Cloudflare dashboard → **Zero Trust → Networks → Tunnels → Create a tunnel** →
   *Cloudflared*, name it `trialshd`, pick **Docker**, and copy the token from the command shown.
2. On Unraid, **Add Container**:
   - Name: `cloudflared-trialshd`, Repository: `cloudflare/cloudflared:latest`
   - Post Arguments: `tunnel --no-autoupdate run --token <YOUR TOKEN>`
3. Back in the tunnel, **Public Hostname → Add**: subdomain `trials`, your domain, service
   **HTTP** `http://<unraid-ip>:8095`.
4. Check `https://trials.<your-domain>/health`.

### Optional: lock it to your game clients (Cloudflare Access)

1. **Zero Trust → Access → Service Auth → Create Service Token**; save the Client ID and Secret.
2. **Access → Applications → Add → Self-hosted**: domain `trials.<your-domain>`, policy action
   **Service Auth**, include the token from step 1.
3. Put the ID and secret in each player's `trialshd.toml` (`trialshd_cf_client_id`,
   `trialshd_cf_client_secret`).

## Game settings (`trialshd.toml` next to the game)

```toml
trialshd_player_name = "Trev"
trialshd_server_url = "https://trials.example.com"
trialshd_player_key = "a-password-only-you-know"
```

The first time a name connects, the server remembers that name's key; after that only the same
key can post times for it. An admin can free a name with
`POST /api/v1/admin/reset_key {"name": "..."}` (header `X-Admin-Key`).

## Ranking

How each leaderboard ("view") is ranked comes from `/data/views.json`:

```json
{ "3": { "rating": 268435457, "order": "asc" } }
```

`rating` is the property id used for ranking, `order` is `asc` (lowest first, e.g. times) or
`desc`. Views not listed are ranked by their first property, lowest first. The server log shows
every view and property it receives, which is how this file gets filled in.

## Friends

By default **everyone who has played on your server is automatically everyone's friend**, so the
in-game Friends leaderboard just works with nothing to set up. To use hand-picked friends instead,
add the variable `TRIALS_EVERYONE_FRIENDS` = `0` to the container and add friends with
`POST /api/v1/friends {"name": "Bob"}` (with your player headers).

## API summary

| Method | Path | Notes |
|---|---|---|
| GET | `/health` | no auth |
| GET | `/api/v1/hello` | registers/checks the player, returns the XUID |
| POST | `/api/v1/stats` | `{"views":[{"view":N,"props":[{"id":..,"type":..,"value":..}]}]}` |
| GET | `/api/v1/leaderboard?view=N&start=R&count=C` or `&around=<xuid>` | `fmt=text` for the game |
| POST | `/api/v1/read` | `{"view":N,"xuids":["..."]}` |
| GET/POST | `/api/v1/friends` | list / add |

All player requests send `X-Player-Name` and `X-Player-Key`.
