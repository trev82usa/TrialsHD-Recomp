# Trials HD leaderboard server on Cloudflare Workers (free plan)

The same server as `../trials_server.py`, rewritten as a Cloudflare Worker with a D1 (SQLite)
database. Nothing runs at home, and the free plan (100,000 requests a day) is far more than a
group of players uses. The game needs no changes; only `trialshd_server_url` points here.

Everything below is done in the Cloudflare dashboard; no tools to install.

## 1. Create the database

**Storage & Databases → D1 SQL Database → Create** → name it `trialshd` → Create.

## 2. Create the Worker

1. **Compute (Workers) → Workers & Pages → Create → Start with Hello World** → name it
   `trialshd` → **Deploy**.
2. **Edit code**, delete everything in `worker.js`, paste in the contents of [worker.js](worker.js),
   then **Deploy**.
3. In the Worker: **Bindings → Add binding → D1 database**. Variable name `DB`, database
   `trialshd` → Add. (The page may redeploy; that's fine.)
4. Optional, under **Settings → Variables and Secrets**:
   - `ADMIN_KEY` (type **Secret**): a long random password for the admin endpoints.
   - `EVERYONE_FRIENDS` = `0` if you want hand-picked friends instead of everyone being friends.
   - `VIEWS_JSON`: ranking overrides, same format as `views.example.json`. Not needed for Trials HD.
5. Open `https://trialshd.<your-account>.workers.dev/health`. It should say `{"ok":true}`.
   Tables are created automatically on first use.

## 3. Use your own address (optional)

**Worker → Settings → Domains & Routes → Add → Custom domain** → e.g. `trials.example.com`.
The domain must be on your Cloudflare account. If that name already points somewhere else (for
example a Cloudflare Tunnel), remove the old public hostname and its DNS record first.

## 4. Bring over times from the Docker server (optional)

Players keep their names, passwords and best times.

1. Copy `export_for_d1.py` next to `trials.db` (on Unraid: into `/mnt/user/appdata/trialshd/`).
2. Run it. On Unraid, click the `trialshd-server` container → **Console**, then:
   ```
   python3 /data/export_for_d1.py /data/trials.db > /data/import.sql
   ```
3. Open `import.sql` in a text editor and copy everything.
4. **D1 → trialshd → Console**, paste, **Execute**.

## Game settings

```toml
trialshd_server_url = "https://trials.example.com"   # or https://trialshd.<account>.workers.dev
```

## Command line instead of the dashboard

With [wrangler](https://developers.cloudflare.com/workers/wrangler/): create the database with
`npx wrangler d1 create trialshd`, put its id in `wrangler.toml`, then `npx wrangler deploy`.
Import old times with `npx wrangler d1 execute trialshd --remote --file import.sql`.
