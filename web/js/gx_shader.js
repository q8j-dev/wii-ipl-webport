// Generates GLSL for the current GX pipeline configuration (see gpu.js
// shaderConfig): transform, color channels (lighting), texture coordinate
// generation, and the TEV combiner stages with alpha test.

const KCSEL = [
  'vec3(1.0)', 'vec3(0.875)', 'vec3(0.75)', 'vec3(0.625)', 'vec3(0.5)', 'vec3(0.375)', 'vec3(0.25)', 'vec3(0.125)',
  'vec3(0.0)', 'vec3(0.0)', 'vec3(0.0)', 'vec3(0.0)',
  'uKonst[0].rgb', 'uKonst[1].rgb', 'uKonst[2].rgb', 'uKonst[3].rgb',
  'uKonst[0].rrr', 'uKonst[1].rrr', 'uKonst[2].rrr', 'uKonst[3].rrr',
  'uKonst[0].ggg', 'uKonst[1].ggg', 'uKonst[2].ggg', 'uKonst[3].ggg',
  'uKonst[0].bbb', 'uKonst[1].bbb', 'uKonst[2].bbb', 'uKonst[3].bbb',
  'uKonst[0].aaa', 'uKonst[1].aaa', 'uKonst[2].aaa', 'uKonst[3].aaa',
];
const KASEL = [
  '1.0', '0.875', '0.75', '0.625', '0.5', '0.375', '0.25', '0.125',
  '0.0', '0.0', '0.0', '0.0', '0.0', '0.0', '0.0', '0.0',
  'uKonst[0].r', 'uKonst[1].r', 'uKonst[2].r', 'uKonst[3].r',
  'uKonst[0].g', 'uKonst[1].g', 'uKonst[2].g', 'uKonst[3].g',
  'uKonst[0].b', 'uKonst[1].b', 'uKonst[2].b', 'uKonst[3].b',
  'uKonst[0].a', 'uKonst[1].a', 'uKonst[2].a', 'uKonst[3].a',
];
const SWZ = 'rgba';

function lightingCode(chan, isAlpha, ctrl, src) {
  // ctrl: chan control register
  const matVtx = ctrl & 1;
  const enable = (ctrl >> 1) & 1;
  const ambVtx = (ctrl >> 6) & 1;
  const diffFn = (ctrl >> 7) & 3;
  const attnSel = (ctrl >> 9) & 1;
  const attnEn = (ctrl >> 10) & 1;
  const mask = ((ctrl >> 2) & 15) | (((ctrl >> 11) & 15) << 4);
  const comp = isAlpha ? '.a' : '.rgb';
  const t = isAlpha ? 'float' : 'vec3';
  const mat = matVtx ? `${src}${comp}` : `uChanMat[${chan}]${comp}`;
  if (!enable) return `${t}(${mat})`;
  const amb = ambVtx ? `${src}${comp}` : `uChanAmb[${chan}]${comp}`;
  let code = `lightSum_${chan}${isAlpha ? 'a' : 'c'}(${amb})`;
  return {mat, code, mask, diffFn, attnSel, attnEn, t, comp};
}

function makeLightFunc(name, l, isAlpha) {
  const comp = isAlpha ? '.a' : '.rgb';
  const t = isAlpha ? 'float' : 'vec3';
  let s = `${t} ${name}(${t} amb) {\n  ${t} sum = amb;\n`;
  for (let i = 0; i < 8; i++) {
    if (!(l.mask & (1 << i))) continue;
    s += `  {\n    vec3 ldir = uLightPos[${i}] - vPosView;\n    float dist2 = dot(ldir, ldir);\n    float dist = sqrt(dist2);\n    ldir = ldir / max(dist, 1e-6);\n`;
    s += `    float nl = dot(vNrmView, ldir);\n`;
    const diff = l.diffFn === 0 ? '1.0' : l.diffFn === 1 ? 'nl' : 'max(nl, 0.0)';
    let attn = '1.0';
    if (l.attnEn) {
      if (l.attnSel) {
        s += `    float cs = max(0.0, dot(ldir, uLightDir[${i}]));\n`;
        s += `    float aat = max(0.0, uLightCos[${i}].x + uLightCos[${i}].y * cs + uLightCos[${i}].z * cs * cs);\n`;
        s += `    float dat = uLightDist[${i}].x + uLightDist[${i}].y * dist + uLightDist[${i}].z * dist2;\n`;
        attn = 'aat / max(dat, 1e-6)';
      } else {
        s += `    float cs = nl > 0.0 ? max(0.0, dot(vNrmView, uLightDir[${i}])) : 0.0;\n`;
        s += `    float aat = max(0.0, uLightCos[${i}].x + uLightCos[${i}].y * cs + uLightCos[${i}].z * cs * cs);\n`;
        s += `    float dat = uLightDist[${i}].x + uLightDist[${i}].y * cs + uLightDist[${i}].z * cs * cs;\n`;
        attn = 'aat / max(dat, 1e-6)';
      }
    }
    s += `    sum += (${attn}) * (${diff}) * uLightColor[${i}]${comp};\n  }\n`;
  }
  s += `  return clamp(sum, 0.0, 1.0);\n}\n`;
  return s;
}

export function buildShaders(c) {
  // ---------------------------------------------------------------- vertex
  let vs = `#version 300 es
precision highp float;
layout(location=0) in vec3 aPos;
layout(location=1) in float aPnmtx;
layout(location=2) in vec3 aNrm;
layout(location=3) in vec3 aTan;
layout(location=4) in vec3 aBin;
layout(location=5) in vec4 aCol0;
layout(location=6) in vec4 aCol1;
layout(location=7) in vec4 aTex01;
layout(location=8) in vec4 aTex23;
layout(location=9) in vec4 aTex45;
layout(location=10) in vec4 aTex67;
layout(location=11) in vec4 aTexMtx0;
layout(location=12) in vec4 aTexMtx1;
uniform vec4 uPosTex[64];
uniform vec3 uNrm[32];
${c.dualTex ? 'uniform vec4 uPost[64];' : ''}
uniform vec4 uProj;
uniform vec2 uProj2;
uniform vec4 uViewport;
uniform vec2 uEfbSize;
uniform vec4 uChanMat[2];
uniform vec4 uChanAmb[2];
uniform vec4 uLightColor[8];
uniform vec3 uLightPos[8];
uniform vec3 uLightDir[8];
uniform vec3 uLightCos[8];
uniform vec3 uLightDist[8];
out vec4 vColor0;
out vec4 vColor1;
${c.texgens.map((_, i) => `out vec3 vTex${i};`).join('\n')}
vec3 vPosView;
vec3 vNrmView;
vec3 mul34(int r, vec4 v) { return vec3(dot(uPosTex[r], v), dot(uPosTex[r + 1], v), dot(uPosTex[r + 2], v)); }
`;
  const chanFuncs = [];
  const chanExpr = [];
  for (let ch = 0; ch < 2; ch++) {
    const src = ch ? 'aCol1' : 'aCol0';
    const cc = lightingCode(ch, false, c.chans[ch].color, src);
    const ca = lightingCode(ch, true, c.chans[ch].alpha, src);
    let colExpr, alphaExpr;
    if (typeof cc === 'string') colExpr = cc;
    else {
      chanFuncs.push(makeLightFunc(`lightSum_${ch}c`, cc, false));
      colExpr = `(${cc.mat}) * ${cc.code}`;
    }
    if (typeof ca === 'string') alphaExpr = ca;
    else {
      chanFuncs.push(makeLightFunc(`lightSum_${ch}a`, ca, true));
      alphaExpr = `(${ca.mat}) * ${ca.code}`;
    }
    chanExpr.push([colExpr, alphaExpr]);
  }
  vs += chanFuncs.join('');
  vs += `void main() {
  int pn = int(aPnmtx + 0.5);
  vec4 p4 = vec4(aPos, 1.0);
  vPosView = mul34(pn, p4);
  int nb = pn & 31;
  vNrmView = normalize(vec3(dot(uNrm[nb], aNrm), dot(uNrm[nb + 1], aNrm), dot(uNrm[nb + 2], aNrm)) + vec3(1e-9));
  vec4 clip;
`;
  if (c.proj) {
    vs += `  clip = vec4(uProj.x * vPosView.x + uProj.y, uProj.z * vPosView.y + uProj.w, uProj2.x * vPosView.z + uProj2.y, 1.0);\n`;
  } else {
    vs += `  clip = vec4(uProj.x * vPosView.x + uProj.y * vPosView.z, uProj.z * vPosView.y + uProj.w * vPosView.z, uProj2.x * vPosView.z + uProj2.y, -vPosView.z);\n`;
  }
  vs += `  float w = clip.w;
  vec2 scr = clip.xy * uViewport.xy + w * uViewport.zw;
  gl_Position = vec4(scr.x * 2.0 / uEfbSize.x - w, w - scr.y * 2.0 / uEfbSize.y, 2.0 * clip.z + w, w);
`;
  const numChan = c.numChan;
  for (let ch = 0; ch < 2; ch++) {
    if (ch < numChan) vs += `  vColor${ch} = vec4(${chanExpr[ch][0]}, ${chanExpr[ch][1]});\n`;
    else vs += `  vColor${ch} = vec4(0.0);\n`;
  }
  vs += `  vec2 tc[8] = vec2[8](aTex01.xy, aTex01.zw, aTex23.xy, aTex23.zw, aTex45.xy, aTex45.zw, aTex67.xy, aTex67.zw);
  float tm[8] = float[8](aTexMtx0.x, aTexMtx0.y, aTexMtx0.z, aTexMtx0.w, aTexMtx1.x, aTexMtx1.y, aTexMtx1.z, aTexMtx1.w);
`;
  c.texgens.forEach((tg, i) => {
    const info = tg.info;
    const projQ = (info >> 1) & 1;
    const abc1 = (info >> 2) & 1;
    const type = (info >> 4) & 7;
    const row = (info >> 7) & 31;
    let srcv;
    switch (row) {
      case 0: srcv = 'aPos'; break;
      case 1: srcv = 'aNrm'; break;
      case 2: srcv = 'vec3(aCol0.rg, 1.0)'; break;
      case 3: srcv = 'aTan'; break;
      case 4: srcv = 'aBin'; break;
      default: srcv = row >= 5 && row <= 12 ? `vec3(tc[${row - 5}], 1.0)` : 'vec3(0.0)';
    }
    if (type === 2 || type === 3) {
      // color texgen: s,t from the color channel
      vs += `  vTex${i} = vec3(vColor${type - 2}.rg, 1.0);\n`;
      return;
    }
    if (type === 1) {
      // emboss: perturb the source coordinates (approximation)
      const src = (info >> 12) & 7;
      vs += `  vTex${i} = vec3(vTex${src}.xy, 1.0);\n`;
      return;
    }
    const v4 = abc1 ? `vec4(${srcv}, 1.0)` : `vec4((${srcv}).xy, 1.0, 1.0)`;
    vs += `  {\n    int m = int(tm[${i}] + 0.5);\n    vec4 s = ${v4};\n`;
    if (projQ) vs += `    vec3 t = vec3(dot(uPosTex[m], s), dot(uPosTex[m + 1], s), dot(uPosTex[m + 2], s));\n`;
    else vs += `    vec3 t = vec3(dot(uPosTex[m], s), dot(uPosTex[m + 1], s), 1.0);\n`;
    if (c.dualTex) {
      const post = tg.post;
      const pidx = post & 63;
      const norm = (post >> 8) & 1;
      if (norm) vs += `    t = normalize(t);\n`;
      vs += `    vec4 t4 = vec4(t, 1.0);\n    t = vec3(dot(uPost[${pidx}], t4), dot(uPost[${pidx + 1}], t4), dot(uPost[${pidx + 2}], t4));\n`;
    }
    vs += `    vTex${i} = t;\n  }\n`;
  });
  vs += `}\n`;

  // -------------------------------------------------------------- fragment
  let fs = `#version 300 es
precision highp float;
in vec4 vColor0;
in vec4 vColor1;
${c.texgens.map((_, i) => `in vec3 vTex${i};`).join('\n')}
uniform sampler2D uTex0, uTex1, uTex2, uTex3, uTex4, uTex5, uTex6, uTex7;
uniform vec4 uTevReg[4];
uniform vec4 uKonst[4];
uniform vec2 uAlphaRef;
out vec4 fragColor;
`;
  const usedMaps = new Set();
  fs += `void main() {
  vec4 r[4];
  r[0] = uTevReg[0]; r[1] = uTevReg[1]; r[2] = uTevReg[2]; r[3] = uTevReg[3];
  vec4 tex = vec4(0.0);
  vec4 ras = vec4(0.0);
  vec3 kc; float ka;
`;
  const uv = (tc) => tc < c.texgens.length ? `(vTex${tc}.xy / (vTex${tc}.z == 0.0 ? 1.0 : vTex${tc}.z))` : 'vec2(0.0)';
  const colIn = ['r[0].rgb', 'r[0].aaa', 'r[1].rgb', 'r[1].aaa', 'r[2].rgb', 'r[2].aaa', 'r[3].rgb', 'r[3].aaa',
    'tex.rgb', 'tex.aaa', 'ras.rgb', 'ras.aaa', 'vec3(1.0)', 'vec3(0.5)', 'kc', 'vec3(0.0)'];
  const alphaIn = ['r[0].a', 'r[1].a', 'r[2].a', 'r[3].a', 'tex.a', 'ras.a', 'ka', '0.0'];
  const scales = ['1.0', '2.0', '4.0', '0.5'];
  const biases = ['0.0', '0.5', '-0.5', '0.0'];
  c.stages.forEach((st, i) => {
    const col = st.color, alp = st.alpha;
    const tswap = c.swaps[(alp >> 2) & 3], rswap = c.swaps[alp & 3];
    const sw = (s) => s.map(x => SWZ[x]).join('');
    fs += `  // stage ${i}\n`;
    if (st.texEnable) {
      usedMaps.add(st.texmap);
      fs += `  tex = texture(uTex${st.texmap}, ${uv(st.texcoord)}).${sw(tswap)};\n`;
    } else {
      fs += `  tex = vec4(1.0);\n`;
    }
    const chan = st.chan;
    const rasSrc = chan === 0 ? 'vColor0' : chan === 1 ? 'vColor1' : 'vec4(0.0)';
    fs += `  ras = clamp(${rasSrc}, 0.0, 1.0).${sw(rswap)};\n`;
    fs += `  kc = ${KCSEL[st.kcsel] || 'vec3(0.0)'}; ka = ${KASEL[st.kasel] || '0.0'};\n`;
    // color
    {
      const d = col & 15, cc = (col >> 4) & 15, b = (col >> 8) & 15, a = (col >> 12) & 15;
      const bias = (col >> 16) & 3, op = (col >> 18) & 1, clampF = (col >> 19) & 1, scale = (col >> 20) & 3, dest = (col >> 22) & 3;
      const A = `clamp(${colIn[a]}, 0.0, 1.0)`, B = `clamp(${colIn[b]}, 0.0, 1.0)`, C = `clamp(${colIn[cc]}, 0.0, 1.0)`, D = colIn[d];
      let expr;
      if (bias === 3) {
        const cmp = (scale << 1) | op;
        expr = `${D} + ${compareColor(cmp, A, B)} * ${C}`;
      } else {
        expr = `(${D} ${op ? '-' : '+'} mix(${A}, ${B}, ${C}) + ${biases[bias]}) * ${scales[scale]}`;
      }
      fs += `  vec3 c${i} = ${clampF ? `clamp(${expr}, 0.0, 1.0)` : `clamp(${expr}, -4.0, 4.0)`};\n`;
      var colDest = dest;
    }
    // alpha
    {
      const d = (alp >> 4) & 7, cc = (alp >> 7) & 7, b = (alp >> 10) & 7, a = (alp >> 13) & 7;
      const bias = (alp >> 16) & 3, op = (alp >> 18) & 1, clampF = (alp >> 19) & 1, scale = (alp >> 20) & 3, dest = (alp >> 22) & 3;
      const A = `clamp(${alphaIn[a]}, 0.0, 1.0)`, B = `clamp(${alphaIn[b]}, 0.0, 1.0)`, C = `clamp(${alphaIn[cc]}, 0.0, 1.0)`, D = alphaIn[d];
      let expr;
      if (bias === 3) {
        const cmp = (scale << 1) | op;
        expr = `${D} + ${compareAlpha(cmp, A, B)} * ${C}`;
      } else {
        expr = `(${D} ${op ? '-' : '+'} mix(${A}, ${B}, ${C}) + ${biases[bias]}) * ${scales[scale]}`;
      }
      fs += `  float a${i} = ${clampF ? `clamp(${expr}, 0.0, 1.0)` : `clamp(${expr}, -4.0, 4.0)`};\n`;
      fs += `  r[${colDest}].rgb = c${i};\n  r[${dest}].a = a${i};\n`;
    }
  });
  fs += `  vec4 prev = clamp(r[0], 0.0, 1.0);\n`;
  // alpha test
  const af = c.alphaFunc;
  const comp0 = (af >> 16) & 7, comp1 = (af >> 19) & 7, logic = (af >> 22) & 3;
  const cmpExpr = (f, ref) => {
    switch (f) {
      case 0: return 'false';
      case 1: return `(ai < ${ref})`;
      case 2: return `(ai == ${ref})`;
      case 3: return `(ai <= ${ref})`;
      case 4: return `(ai > ${ref})`;
      case 5: return `(ai != ${ref})`;
      case 6: return `(ai >= ${ref})`;
      default: return 'true';
    }
  };
  if (!(comp0 === 7 && comp1 === 7)) {
    const e0 = cmpExpr(comp0, 'r0'), e1 = cmpExpr(comp1, 'r1');
    const ops = ['&&', '||', '!=', '=='];
    fs += `  int ai = int(prev.a * 255.0 + 0.5);\n  int r0 = int(uAlphaRef.x * 255.0 + 0.5);\n  int r1 = int(uAlphaRef.y * 255.0 + 0.5);\n`;
    fs += `  if (!(${e0} ${ops[logic]} ${e1})) discard;\n`;
  }
  fs += `  fragColor = prev;\n}\n`;
  return {vs, fs, usedMaps};
}

function compareColor(cmp, A, B) {
  const r8 = (x) => `floor(${x}.r * 255.0 + 0.5)`;
  const gr16 = (x) => `(floor(${x}.g * 255.0 + 0.5) * 256.0 + floor(${x}.r * 255.0 + 0.5))`;
  const bgr24 = (x) => `(floor(${x}.b * 255.0 + 0.5) * 65536.0 + floor(${x}.g * 255.0 + 0.5) * 256.0 + floor(${x}.r * 255.0 + 0.5))`;
  switch (cmp) {
    case 0: return `(${r8(A)} > ${r8(B)} ? 1.0 : 0.0)`;
    case 1: return `(${r8(A)} == ${r8(B)} ? 1.0 : 0.0)`;
    case 2: return `(${gr16(A)} > ${gr16(B)} ? 1.0 : 0.0)`;
    case 3: return `(${gr16(A)} == ${gr16(B)} ? 1.0 : 0.0)`;
    case 4: return `(${bgr24(A)} > ${bgr24(B)} ? 1.0 : 0.0)`;
    case 5: return `(${bgr24(A)} == ${bgr24(B)} ? 1.0 : 0.0)`;
    case 6: return `vec3(greaterThan(floor(${A} * 255.0 + 0.5), floor(${B} * 255.0 + 0.5)))`;
    default: return `vec3(equal(floor(${A} * 255.0 + 0.5), floor(${B} * 255.0 + 0.5)))`;
  }
}

function compareAlpha(cmp, A, B) {
  const a8 = (x) => `floor(${x} * 255.0 + 0.5)`;
  if (cmp === 6) return `(${a8(A)} > ${a8(B)} ? 1.0 : 0.0)`;
  if (cmp === 7) return `(${a8(A)} == ${a8(B)} ? 1.0 : 0.0)`;
  // The wider compares use the color channels of the inputs; approximate with alpha.
  return (cmp & 1) ? `(${a8(A)} == ${a8(B)} ? 1.0 : 0.0)` : `(${a8(A)} > ${a8(B)} ? 1.0 : 0.0)`;
}
