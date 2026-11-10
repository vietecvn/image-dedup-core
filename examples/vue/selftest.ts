import { createImageDedupClient, findDuplicate } from '@/utils/imageDedup/imageDedupClient'

const out = document.querySelector('#app')!
const client = createImageDedupClient({ idleTimeoutMs: 1000 })
const res: Record<string, unknown> = {}
for (const name of ['base.jpg', 'p3.jpg', 'lossy.webp']) {
  const blob = await (await fetch(`fx/${name}`)).blob()
  const r = await client.compress(new File([blob], name))
  res[name] = r.ok ? `${r.engine} ${r.width}x${r.height} ${r.outputSha256?.slice(0, 16)} p3=${r.colorConverted}` : r.code
  if (r.ok && name === 'base.jpg' && r.fingerprint && r.outputSha256) {
    const posted = await client.fingerprintUrl(URL.createObjectURL(r.file))
    res.self = posted.ok ? findDuplicate({ sha256: r.outputSha256, fingerprint: r.fingerprint }, [{ id: 1, sha256: posted.sha256, fingerprint: posted.fingerprint }])?.kind : posted.code
  }
}
// Gọi song song: client phải tự xếp hàng.
const both = await Promise.all([client.compress(new File([await (await fetch('fx/base.jpg')).blob()], 'x.jpg')), client.fingerprint(new Blob(['abc']))])
res.parallel = both.map(r => (r.ok ? 'ok' : r.code)).join(',')
await new Promise(r => setTimeout(r, 1500))
const again = await client.compress(new File([await (await fetch('fx/lossy.webp')).blob()], 'y.webp'))
res.afterIdle = again.ok ? again.outputSha256?.slice(0, 16) : again.code
out.textContent = JSON.stringify(res, null, 1)
;(window as unknown as { __done: boolean }).__done = true
