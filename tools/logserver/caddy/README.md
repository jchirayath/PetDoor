# The reference deployment's Caddy vhost

`petdoor.aspl.net.caddy` is **the file that is actually running**, not an example
— copied verbatim from the reference deployment so that it is version-controlled
rather than existing only on one VM. It was previously only on that VM, and would
have been lost with it.

[../../../docs/WEB-DASHBOARD.md](../../../docs/WEB-DASHBOARD.md) has the generic
pattern, for nginx and Apache too. Read that first if you are standing up your
own; this file is the worked example, including the parts that are specific to one
host (an `entra-auth` import, a `petdoor:8080` container name) and that you will
have to replace.

## Why it is worth reading even if you use nginx

Three things in here were learned the hard way and are not obvious:

**One `route` block, not bare directives.** Caddy sorts bare directives into its
own canonical order, so `request_header -X-...` written *before* `forward_auth`
would run *after* it and delete the identity just established. `route` preserves
written order.

**`/ingest` must bypass the identity provider entirely.** oauth2-proxy answers an
unauthenticated request with an HTML sign-in redirect, and a microcontroller
cannot satisfy that — it would retry forever. The door authenticates at the
application layer with HMAC-SHA256 instead.

**Public routes are listed explicitly, never by exclusion.** Everything not named
falls through to the private catch-all, so a route added later is private by
default and becomes public only on purpose. The door's event log says when a house
is reliably empty; that is what the gate is for.

## Changing it

Validate before you reload. This Caddy serves 26 vhosts, and a bad config takes
all of them down:

```bash
cd ~/webhost
sudo docker compose exec -T caddy caddy adapt --config /etc/caddy/Caddyfile   # FIRST
sudo docker compose exec -T caddy caddy reload --config /etc/caddy/Caddyfile
```

Then verify the split still holds — the data, not just the page:

```bash
curl -o /dev/null -w '%{http_code}\n' https://petdoor.aspl.net/api/events   # must be 302
curl -o /dev/null -w '%{http_code}\n' -X POST -d '{}' \
     http://petdoor.aspl.net/ingest                                          # must be 401
```

A 200 on the first means the household's routine is public. A redirect on the
second means the door has been locked out of its own log server.

## Keeping this file and the server in step

There is no automation. After editing the live config, copy it back here in the
same change:

```bash
az ssh config --file /tmp/azcfg -g RG-WEBHOST -n webhost --overwrite
ssh -F /tmp/azcfg RG-WEBHOST-webhost \
  'sudo cat /srv/webhost/sites/petdoor.aspl.net.caddy' \
  > tools/logserver/caddy/petdoor.aspl.net.caddy
```
