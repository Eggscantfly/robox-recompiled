/**
 * Serves the Emscripten build out of an R2 bucket under a path prefix on an
 * existing site, so the main site (Squarespace) keeps serving everything else.
 *
 * Why a Worker and not Pages: robox.data is ~42 MB and Cloudflare Pages caps
 * a single file at 25 MiB. R2 has no per-object cap of that kind, but nothing
 * in front of R2 sets the two headers below, so a Worker has to sit in the
 * middle anyway.
 */

const PREFIX = '/robox';

// Explicit, because R2 stores whatever content-type the upload declared and a
// wrong one on the .wasm silently disables streaming compilation.
const TYPES = {
    html: 'text/html; charset=utf-8',
    js: 'text/javascript; charset=utf-8',
    wasm: 'application/wasm',
    data: 'application/octet-stream',
};

/**
 * Cross-origin isolation. The build is -pthread/-sPROXY_TO_PTHREAD, so it
 * needs SharedArrayBuffer, which browsers only expose to isolated pages.
 * Without these the game does not boot at all -- this is not a hardening
 * nicety, it is load-bearing.
 */
function isolate(headers) {
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    headers.set('Cross-Origin-Resource-Policy', 'same-origin');
    return headers;
}

function fail(status, message) {
    return new Response(message, {
        status,
        headers: isolate(new Headers({ 'Content-Type': 'text/plain; charset=utf-8' })),
    });
}

export default {
    async fetch(request, env) {
        const url = new URL(request.url);

        if (request.method !== 'GET' && request.method !== 'HEAD') {
            const h = isolate(new Headers({ Allow: 'GET, HEAD' }));
            return new Response('Method not allowed', { status: 405, headers: h });
        }

        // /robox -> /robox/ so the relative <script src=ROBOX.js> in the shell
        // resolves against the directory and not against the site root.
        if (url.pathname === PREFIX) {
            return Response.redirect(`${url.origin}${PREFIX}/`, 301);
        }
        if (!url.pathname.startsWith(`${PREFIX}/`)) {
            return fail(404, 'Not found');
        }

        let key = decodeURIComponent(url.pathname.slice(PREFIX.length + 1));
        if (key === '' || key.endsWith('/')) key += 'index.html';
        if (key.includes('..')) return fail(400, 'Bad request');

        // Only hand R2 the headers as a range when the client actually asked
        // for one. Passing them unconditionally makes R2 report a range on
        // every hit, which turns plain GETs into 206s -- and a 206 on the
        // document or the .wasm breaks streaming compilation and confuses
        // caches. onlyIf is safe to pass either way.
        const wantsRange = request.headers.has('Range');
        const object = await env.BUCKET.get(key, {
            range: wantsRange ? request.headers : undefined,
            onlyIf: request.headers,
        });

        if (object === null) return fail(404, `Not found: ${key}`);

        const headers = new Headers();
        object.writeHttpMetadata(headers);
        headers.set('ETag', object.httpEtag);
        headers.set('Accept-Ranges', 'bytes');

        const ext = key.split('.').pop().toLowerCase();
        if (TYPES[ext]) headers.set('Content-Type', TYPES[ext]);

        // no-cache means "keep it, but revalidate before every use" -- NOT
        // "do not store". The browser holds all 59 MB and spends one
        // conditional request per file per load, which R2 answers with a 304.
        //
        // A max-age was tried first and was wrong: these filenames carry no
        // content hash, so after a redeploy a returning visitor got the new
        // .wasm with a cached old .js until the age expired. That combination
        // does not fail loudly -- it aborts somewhere inside the runtime with
        // an error about the previous build. Correctness beats one round trip.
        headers.set('Cache-Control', 'no-cache');
        isolate(headers);

        // A body-less object means the If-None-Match/If-Modified-Since
        // precondition held: nothing changed.
        if (!('body' in object) || object.body === null) {
            return new Response(null, { status: 304, headers });
        }

        let status = 200;
        if (wantsRange && object.range) {
            const size = object.size;
            let start, length;
            if ('suffix' in object.range) {
                length = object.range.suffix;
                start = size - length;
            } else {
                start = object.range.offset ?? 0;
                length = object.range.length ?? size - start;
            }
            status = 206;
            headers.set('Content-Range', `bytes ${start}-${start + length - 1}/${size}`);
        }

        return new Response(request.method === 'HEAD' ? null : object.body, { status, headers });
    },
};
