/**
 * So trùng dưới client: ảnh mới vừa nén so với các ảnh đã đăng (tải về từ server,
 * tính dấu vân tay bằng `client.fingerprint(...)`). TS thuần, không cần WASM —
 * cùng công thức và ngưỡng với `ihd_compare` bên C và `findDuplicate` bên Dart.
 */

/** Ngưỡng (số bit lệch trên 64) — PHẢI giữ giống src/image_dedup.cpp. */
export const DEDUP_THRESHOLDS = {
  duplicatePhash: 6,
  duplicateDhash: 10,
  similarPhash: 12,
  similarDhash: 16,
} as const;

export type DedupVerdict = 'different' | 'similar' | 'duplicate';

export interface DedupMatch {
  phashDistance: number;
  dhashDistance: number;
  verdict: DedupVerdict;
}

/** Một ảnh đã biết (ảnh đã đăng): sha256 của file + dấu vân tay. */
export interface KnownImage {
  id: string | number;
  sha256: string;
  fingerprint: string;
}

export interface DuplicateHit {
  image: KnownImage;
  /** 'exact' = cùng từng byte file; 'duplicate'/'similar' = theo dấu vân tay. */
  kind: 'exact' | 'duplicate' | 'similar';
  match: DedupMatch | null;
}

const BITS = [0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4];

/** Số bit khác nhau giữa hai chuỗi hex cùng độ dài. */
const hammingHex = (a: string, b: string): number => {
  let distance = 0;
  for (let i = 0; i < a.length; i += 1) {
    distance += BITS[parseInt(a[i], 16) ^ parseInt(b[i], 16)];
  }
  return distance;
};

const isFingerprint = (hex: string) => /^[0-9a-f]{32}$/i.test(hex);

/** So hai dấu vân tay hex 32 ký tự (16 đầu pHash, 16 sau dHash). */
export const compareFingerprints = (a: string, b: string): DedupMatch | null => {
  if (!isFingerprint(a) || !isFingerprint(b)) return null;
  const phashDistance = hammingHex(a.slice(0, 16).toLowerCase(), b.slice(0, 16).toLowerCase());
  const dhashDistance = hammingHex(a.slice(16).toLowerCase(), b.slice(16).toLowerCase());
  const t = DEDUP_THRESHOLDS;
  let verdict: DedupVerdict = 'different';
  if (phashDistance <= t.duplicatePhash && dhashDistance <= t.duplicateDhash) {
    verdict = 'duplicate';
  } else if (phashDistance <= t.similarPhash && dhashDistance <= t.similarDhash) {
    verdict = 'similar';
  }
  return { phashDistance, dhashDistance, verdict };
};

/**
 * Tìm ảnh đã đăng trùng với ảnh mới. Ưu tiên trùng tuyệt đối (sha256), rồi tới
 * dấu vân tay gần nhất. Trả null nếu không có ảnh nào trùng hoặc nghi trùng.
 */
export const findDuplicate = (
  candidate: { sha256: string; fingerprint: string },
  known: readonly KnownImage[],
): DuplicateHit | null => {
  const exact = known.find(image => image.sha256 && image.sha256 === candidate.sha256);
  if (exact) return { image: exact, kind: 'exact', match: null };

  let best: DuplicateHit | null = null;
  for (const image of known) {
    const match = compareFingerprints(candidate.fingerprint, image.fingerprint);
    if (!match || match.verdict === 'different') continue;
    const score = match.phashDistance + match.dhashDistance;
    const bestScore = best?.match ? best.match.phashDistance + best.match.dhashDistance : Infinity;
    if (score < bestScore) best = { image, kind: match.verdict, match };
  }
  return best;
};
