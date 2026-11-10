/**
 * Khai báo kiểu cho file do Emscripten sinh (scripts/build_wasm.sh). File .mjs là
 * JS đã nén kèm wasm base64 nên TypeScript không tự suy kiểu được; các hàm thật
 * được mô tả ở interface `WasmModule` trong imageDedup.ts.
 */
declare const createImageDedupModule: (moduleOverrides?: Record<string, unknown>) => Promise<unknown>;
export default createImageDedupModule;
