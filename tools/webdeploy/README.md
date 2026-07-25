# Deploying the web build to `yourdomain.com/robox`

The main site stays on Squarespace. Cloudflare intercepts one path prefix and
serves the game out of R2. Squarespace is never touched and never knows.

## Why it is built this way

Three constraints force the shape:

1. **Squarespace cannot host it.** There is no way to upload loose files at a
   path, and the build needs two response headers Squarespace cannot set.
2. **The build needs cross-origin isolation.** It links `-pthread
   -sPROXY_TO_PTHREAD=1`, so it needs `SharedArrayBuffer`, which browsers only
   expose when the page is served with `Cross-Origin-Opener-Policy:
   same-origin` and `Cross-Origin-Embedder-Policy: require-corp`. Missing
   either one and the game does not boot. This is also why GitHub Pages is not
   an option.
3. **`ROBOX.data` is ~42 MB.** Cloudflare Pages caps one file at 25 MiB, so
   Pages is out. R2 has no such cap, but nothing in front of R2 sets the
   headers from (2), so a Worker sits in the middle and does both jobs.

## The filename trap

The build emits mixed casing. `robox.html` fetches `ROBOX.js` and `robox.js`
fetches `ROBOX.data`, but the files on disk are lowercase — NTFS preserves the
original casing when a file is overwritten, and `OUTPUT_NAME "ROBOX"` arrived
after those files were first created. Locally nothing breaks. On R2, or any
case-sensitive host, both are a 404 and the page sits on `loading…` forever.

`stage.ps1` renames them to what the runtime actually asks for, and then
asserts that every `ROBOX.*` string it can find in the html and js resolves to
a staged file. Do not skip it.

## One-time setup

1. **Enable R2** in the Cloudflare dashboard (R2 → Overview). It is free at
   this size — 10 GB storage and free egress — but Cloudflare still requires a
   card on file to switch it on.
2. **Log in:** `npx wrangler login`
3. **Edit `wrangler.toml`** — replace `example.com` in all three places with
   the real domain. `zone_name` is the apex, as listed in the Cloudflare
   dashboard, even if the site is served from `www`.
4. **Check the orange cloud.** In Cloudflare DNS, the record the site is served
   from must be **Proxied**, not DNS-only. A Worker route on a grey-cloud
   record never fires — the request goes straight to Squarespace, which answers
   `/robox` with its own 404. This is the single most likely reason a deploy
   "succeeds" and the URL still 404s.
5. **Check SSL mode.** Cloudflare SSL/TLS must be **Full** or **Full (strict)**.
   On *Flexible*, Squarespace forces HTTPS and you get a redirect loop that
   takes down the main site. Squarespace does not officially support running
   behind Cloudflare's proxy, so verify the main site still loads immediately
   after flipping the record to proxied — and flip it back if it does not.

## Deploy

```powershell
pwsh tools/webdeploy/stage.ps1                    # rename + verify
pwsh tools/webdeploy/upload.ps1 -CreateBucket     # first run only
npx wrangler deploy --config tools/webdeploy/wrangler.toml
```

Subsequent deploys drop `-CreateBucket`. Re-uploading is ~59 MB each time; only
the changed files need pushing if you would rather do it by hand.

## Verifying

```powershell
curl.exe -I https://yourdomain.com/robox/
```

Expect `200`, `content-type: text/html`, and **both** COOP and COEP headers. If
the headers are missing you are being served by Squarespace, not the Worker —
go back to step 4.

In the browser, DevTools → Console should show no 404s, and
`crossOriginIsolated` should evaluate to `true`.

## Local testing without a Cloudflare account

`wrangler` simulates R2 locally, so the whole thing runs offline:

```powershell
$P = "$env:TEMP\robox-r2"
npx wrangler r2 object put robox-web/index.html --file build-web-deploy/index.html --content-type "text/html; charset=utf-8" --local --persist-to $P --config tools/webdeploy/wrangler.toml
# ...repeat for ROBOX.js, ROBOX.wasm, ROBOX.data with their content types...
npx wrangler dev --config tools/webdeploy/wrangler.toml --persist-to $P --port 8787
```

Then open `http://localhost:8787/robox/`.

## Known gaps

- **No audio on web.** WebAudio is main-thread only and the guest never returns
  to a host loop; it needs an AudioWorklet plus a click gesture. Silent by
  design, not a deploy fault.
- **The build is a debug build.** It links `-sASSERTIONS=1` and
  `--profiling-funcs`, which together cost roughly a megabyte of wasm and some
  runtime checking, in exchange for readable guest backtraces in the crash
  overlay. Worth dropping both for a build meant for strangers.
