// ---------------------------------------------------------------- 1.83: rain falling (NewLook.hlsl: VS_Rain, VS_Splash)
// Drawn in the air pass (after the game's transparents, before its post-processing) onto the scene: 2 x 24000 streak
// slots (near and far lattices round the camera; as many fall as the weather's rain x Rain), 6000 splash slots. Our
// constants (b5 LightCB, b13 the rain map's) and this frame's view ray record (t79) bound to the vertex stage too;
// the rain map (t51) there: no rain under roofs. Blended dest x (1 - a) + c (colour only).
static ID3D11BlendState* g_rfBlend;
static const UINT kRfStreaks = 48000, kRfSplashes = 6000;
static void RainfallDraw(ID3D11DeviceContext* ctx, int cur, ID3D11ShaderResourceView* depth)
{
    float R = g_wxRain > 0 ? g_wxRain : 0.0f;
    if (R < 0.01f || !g_vsRain || !g_psRain || !g_vsSplash || !g_psSplash || g_camFrame[cur] != g_frame + 1 || !g_rainCb) return;
    bool streaks = g_p[217].v > 0, splashes = g_p[220].v > 0;
    if (!streaks && !splashes) return;
    if (!g_rfBlend) {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = 7;
        if (FAILED(g_dev->CreateBlendState(&bd, &g_rfBlend))) { g_rfBlend = 0; return; }
    }
    // save what we set beyond the air pass's own saving (the vertex stage's b5 / b13, t51 / t79; our pixel slots)
    ID3D11Buffer* sVcb[2] = {}; ctx->VSGetConstantBuffers(5, 1, &sVcb[0]); ctx->VSGetConstantBuffers(13, 1, &sVcb[1]);
    ID3D11ShaderResourceView* sVsrv[3] = {}; ctx->VSGetShaderResources(51, 1, &sVsrv[0]); ctx->VSGetShaderResources(79, 1, &sVsrv[1]); ctx->VSGetShaderResources(17, 1, &sVsrv[2]);
    ID3D11ShaderResourceView* sPsrv[3] = {}; ctx->PSGetShaderResources(67, 2, sPsrv); ctx->PSGetShaderResources(79, 1, &sPsrv[2]);
    ID3D11ShaderResourceView* sBg = 0; ctx->PSGetShaderResources(80, 1, &sBg);
    ID3D11BlendState* sBl = 0; FLOAT sBf[4]; UINT sBm; ctx->OMGetBlendState(&sBl, sBf, &sBm);
    ID3D11VertexShader* sVs = 0; ctx->VSGetShader(&sVs, 0, 0);   // (the air pass replays the game's full-screen draw after)
    ID3D11InputLayout* sIl = 0; ctx->IAGetInputLayout(&sIl);
    D3D11_PRIMITIVE_TOPOLOGY sTop; ctx->IAGetPrimitiveTopology(&sTop);
    // ours
    RainBind(ctx);   // b13, t51 (the pixel stage)
    ctx->VSSetConstantBuffers(5, 1, &g_cb);
    ctx->VSSetConstantBuffers(13, 1, &g_rainCb);
    ID3D11ShaderResourceView* rm = g_rainCur >= 0 ? g_rainSrv[g_rainCur] : 0;
    ctx->VSSetShaderResources(51, 1, &rm);
    ctx->VSSetShaderResources(79, 1, &g_camRec[cur].srv);
    ctx->VSSetShaderResources(17, 1, &depth);   // (1.84: the splashes on the surface seen)
    // (1.88b: the game's headlights for the vertex stage: their constants b8, pattern t43, the linear sampler s13)
    ID3D11Buffer* sHb = 0; ctx->VSGetConstantBuffers(8, 1, &sHb);
    ID3D11ShaderResourceView* sHt = 0; ctx->VSGetShaderResources(43, 1, &sHt);
    ID3D11SamplerState* sHs = 0; ctx->VSGetSamplers(13, 1, &sHs);
    ctx->VSSetConstantBuffers(8, 1, &g_hlCb); ctx->VSSetShaderResources(43, 1, &g_hlTex); ctx->VSSetSamplers(13, 1, &g_sText);
    ctx->PSSetShaderResources(79, 1, &g_camRec[cur].srv);
    ctx->PSSetShaderResources(17, 1, &depth);
    ctx->PSSetShaderResources(80, 1, &g_hrt[6].srv);   // the scene with its mips (the light round a drop)
    ID3D11ShaderResourceView* sky[2] = { g_panoSrv[g_panoLast], g_probeSrv };
    ctx->PSSetShaderResources(67, 2, sky);
    ID3D11SamplerState* smp[3] = { g_sPoint, g_sWrap, g_sText };
    ctx->PSSetSamplers(11, 3, smp);
    ctx->IASetInputLayout(0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->OMSetBlendState(g_rfBlend, 0, 0xffffffff);
    if (splashes) {
        ctx->VSSetShader(g_vsSplash, 0, 0);
        oPSSetShader(ctx, g_psSplash, 0, 0);
        oDrawInstanced(ctx, 4, kRfSplashes, 0, 0);
    }
    if (streaks) {
        ctx->VSSetShader(g_vsRain, 0, 0);
        oPSSetShader(ctx, g_psRain, 0, 0);
        oDrawInstanced(ctx, 4, kRfStreaks, 0, 0);
    }
    // back
    ctx->VSSetConstantBuffers(5, 1, &sVcb[0]); ctx->VSSetConstantBuffers(13, 1, &sVcb[1]);
    ctx->VSSetShaderResources(51, 1, &sVsrv[0]); ctx->VSSetShaderResources(79, 1, &sVsrv[1]); ctx->VSSetShaderResources(17, 1, &sVsrv[2]);
    ctx->VSSetConstantBuffers(8, 1, &sHb); ctx->VSSetShaderResources(43, 1, &sHt); ctx->VSSetSamplers(13, 1, &sHs);
    if (sHb) sHb->Release(); if (sHt) sHt->Release(); if (sHs) sHs->Release();
    ctx->PSSetShaderResources(67, 2, sPsrv); ctx->PSSetShaderResources(79, 1, &sPsrv[2]); ctx->PSSetShaderResources(80, 1, &sBg);
    ctx->OMSetBlendState(sBl, sBf, sBm);
    ctx->VSSetShader(sVs, 0, 0); ctx->IASetInputLayout(sIl); ctx->IASetPrimitiveTopology(sTop);
    IUnknown* rel[] = { sVcb[0], sVcb[1], sVsrv[0], sVsrv[1], sVsrv[2], sPsrv[0], sPsrv[1], sPsrv[2], sBg, sBl, sVs, sIl };
    for (int i = 0; i < (int)(sizeof(rel) / sizeof(rel[0])); ++i) if (rel[i]) rel[i]->Release();
}

// ---------------------------------------------------------------- 1.84: rain on the camera's lens
// A few hundred drops on the lens, in screen heights (x 0..aspect, y 0..1 down), simulated here once a frame (weather.inc
// calls it): they land by the rain's flux into the lens (the rain's fall and the wind less the camera's own motion, into
// its view: few standing in still rain, many driving into it, more looking up), small (6 .. 23 px at 1440), any size
// by LensDropSize; gravity along the screen (the camera's tilt) moves the big ones (held by their rim ~ 1 / r^2), they lag
// behind the camera's acceleration, the air past a moving camera blows them outwards (~ v^2 / r: off the lens at
// speed); touching ones merge; with no rain they dry. Drawn after the rain (LensDraw): PS_Lens.
static const int kLdMax = 400;
struct LensDrop { float x, y, r, vx, vy, px, py; bool on; };
static LensDrop g_ld[kLdMax];
static ID3D11Buffer* g_ldCb;
static float g_ldAspect = 16.0f / 9.0f;
static unsigned g_ldRnd = 0x1234567u;
static float LdRnd() { g_ldRnd ^= g_ldRnd << 13; g_ldRnd ^= g_ldRnd >> 17; g_ldRnd ^= g_ldRnd << 5; return (g_ldRnd >> 8) / 16777216.0f; }
static void LensTick(float dt)
{
    if (dt <= 0) return;
    float amt = g_p[230].v > 0.5f ? g_p[228].v : 0.0f, R = g_wxRain > 0 ? g_wxRain : 0.0f, size = g_p[229].v;   // (1.86: LensDropsOn)
    if (g_bbW && g_bbH) g_ldAspect = (float)g_bbW / g_bbH;
    if (!g_camKnown) return;
    // the camera's axes from its record (A + B u + C v: the view ray; B to the right, C down the screen)
    // (1.86: the game's record points back (RfClip in NewLook.hlsl): forward, right and down the other way; its w tells)
    float f[3], rt[3], dn[3];
    float wsum = g_cam[0][3] + 0.5f * g_cam[1][3] + 0.5f * g_cam[2][3], sg = (wsum > 0.5f || wsum == 0) ? -1.0f : 1.0f;
    for (int j = 0; j < 3; ++j) { f[j] = sg * (g_cam[0][j] + 0.5f * g_cam[1][j] + 0.5f * g_cam[2][j]); rt[j] = sg * g_cam[1][j]; dn[j] = sg * g_cam[2][j]; }
    auto nrm = [](float* v) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-6f) for (int j = 0; j < 3; ++j) v[j] /= l; };
    nrm(f); nrm(rt); nrm(dn);
    auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    // the camera's acceleration (its velocity's change), smoothed
    static float lastV[3], acc[3];
    for (int j = 0; j < 3; ++j) { float a = (g_camVel[j] - lastV[j]) / dt; if (fabsf(a) > 200) a = 0; acc[j] += (a - acc[j]) * (1 - expf(-dt / 0.1f)); lastV[j] = g_camVel[j]; }
    // landing: the rain past the camera, into its view
    float rv[3] = { g_wind[0] - g_camVel[0], -8.0f * g_p[226].v - g_camVel[1], g_wind[2] - g_camVel[2] };
    float flux = -dot3(rv, f);
    flux = (flux > 0 ? flux : 0) + 0.6f;   // (+ the eddies round the camera: a few drops in still rain too)
    float expect = amt * R * flux * 2.5f * dt;
    while (expect > 0 && LdRnd() < expect) {
        expect -= 1;
        for (int i = 0; i < kLdMax; ++i) if (!g_ld[i].on) {
            LensDrop& d = g_ld[i];
            float u = LdRnd();
            d.x = LdRnd() * g_ldAspect; d.y = LdRnd(); d.r = (0.004f + 0.012f * u * u) * size;
            // (1.85: fewer in the middle of the view: there 15 % as many)
            float cx = (d.x - g_ldAspect * 0.5f) / (g_ldAspect * 0.5f), cy = (d.y - 0.5f) / 0.5f, ce = sqrtf(cx * cx + cy * cy);
            float keep = 0.15f + 0.85f * fminf(fmaxf((ce - 0.25f) / 0.5f, 0.0f), 1.0f);
            if (LdRnd() > keep) break;
            d.vx = d.vy = 0; d.px = d.x; d.py = d.y; d.on = true;
            break;
        }
    }
    // the push along the screen (screen heights / s2): gravity (the camera's tilt), inertia, the air past it
    float g[3] = { 0, -9.81f, 0 };
    float gx = dot3(g, rt) * 0.05f, gy = dot3(g, dn) * 0.05f;
    float ax = -dot3(acc, rt) * 0.03f, ay = -dot3(acc, dn) * 0.03f;
    float fwd = dot3(g_camVel, f), air = fwd > 0 ? fwd * fwd * 2.5e-5f : 0;
    static float tw;
    tw += dt;
    for (int i = 0; i < kLdMax; ++i) {
        LensDrop& d = g_ld[i];
        if (!d.on) continue;
        // the air past a moving camera: out to the sides and down (1.84 pushed outwards every way: drops above the middle
        // went up)
        float ox = d.x - g_ldAspect * 0.5f, ol = fabsf(ox) + 0.1f;
        float fx = gx + ax + air * (ox / ol) / d.r, fy = gy + ay + air * 0.35f / d.r;
        // running, a drop wanders a little (the glass's own unevenness)
        if (d.vx * d.vx + d.vy * d.vy > 1e-6f) fx += sinf(tw * 7.3f + i * 1.7f) * 0.15f;
        float hold = 0.5f * powf(0.009f / d.r, 2);   // (1.85: from 0.016: big drops run now)
        float sp = sqrtf(d.vx * d.vx + d.vy * d.vy), fl = sqrtf(fx * fx + fy * fy);
        d.px = d.x; d.py = d.y;
        if (sp < 1e-3f && fl < hold) { d.vx = d.vy = 0; }
        else {
            float dx = sp > 1e-3f ? d.vx / sp : fx / fl, dy = sp > 1e-3f ? d.vy / sp : fy / fl;
            d.vx += (fx - dx * hold * 0.5f) * dt; d.vy += (fy - dy * hold * 0.5f) * dt;
            float damp = expf(-dt * (3 + 0.01f / d.r));
            d.vx *= damp; d.vy *= damp;
            if (d.vx * dx + d.vy * dy < 0) d.vx = d.vy = 0;
            float v = sqrtf(d.vx * d.vx + d.vy * d.vy);
            if (v > 3) { d.vx *= 3 / v; d.vy *= 3 / v; }
            d.x += d.vx * dt; d.y += d.vy * dt;
            d.r -= sqrtf(d.vx * d.vx + d.vy * d.vy) * dt * 0.004f;   // (a little left on its track)
        }
        if (R < 0.01f) d.r -= 0.0003f * dt;   // drying
        else d.r += 0.0004f * R * (flux + 1) * dt * (0.5f + LdRnd());   // (1.85: the rain landing on it: it grows, then runs)
        if (d.r < 0.002f || d.x < -0.05f || d.x > g_ldAspect + 0.05f || d.y < -0.05f || d.y > 1.05f) d.on = false;
    }
    // touching ones merge (the bigger keeps its place, takes the water and the motion)
    for (int i = 0; i < kLdMax; ++i) {
        if (!g_ld[i].on) continue;
        for (int j = i + 1; j < kLdMax && g_ld[i].on; ++j) {
            LensDrop &a = g_ld[i], &b = g_ld[j];
            if (!b.on) continue;
            float dx = a.x - b.x, dy = a.y - b.y, rs = (a.r + b.r) * 0.85f;
            if (dx * dx + dy * dy > rs * rs) continue;
            LensDrop& hi = a.r >= b.r ? a : b;   // (not "small": a macro in the Windows headers)
            LensDrop& lo = a.r >= b.r ? b : a;
            float vb = hi.r * hi.r * hi.r, vs = lo.r * lo.r * lo.r, vt = vb + vs;
            hi.vx = (hi.vx * vb + lo.vx * vs) / vt; hi.vy = (hi.vy * vb + lo.vy * vs) / vt;
            hi.r = fminf(cbrtf(vt), 0.045f);
            lo.on = false;
        }
    }
}
static void LensDraw(ID3D11DeviceContext* ctx)
{
    if (!g_vsLens || !g_psLens || g_p[228].v <= 0 || g_p[230].v < 0.5f || !g_rfBlend) return;
    static float data[kLdMax * 8];
    int n = 0;
    for (int i = 0; i < kLdMax; ++i) {
        const LensDrop& d = g_ld[i];
        if (!d.on) continue;
        float* q = data + n * 8;
        // its trail: back along its path, up to 6 x its radius (a running drop's streak)
        float tx = (d.px - d.x) * 4, ty = (d.py - d.y) * 4, tl = sqrtf(tx * tx + ty * ty), tm = d.r * 6;
        if (tl > tm) { tx *= tm / tl; ty *= tm / tl; }
        q[0] = d.x / g_ldAspect; q[1] = d.y; q[2] = d.r; q[3] = g_ldAspect;
        q[4] = tx; q[5] = ty; q[6] = 1; q[7] = 0;
        ++n;
    }
    if (!n) return;
    if (!g_ldCb) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(data); bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_dev->CreateBuffer(&bd, 0, &g_ldCb))) { g_ldCb = 0; return; }
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_ldCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, data, n * 32);
    ctx->Unmap(g_ldCb, 0);
    // save
    ID3D11Buffer* sVcb = 0; ctx->VSGetConstantBuffers(10, 1, &sVcb);
    ID3D11VertexShader* sVs = 0; ctx->VSGetShader(&sVs, 0, 0);
    ID3D11InputLayout* sIl = 0; ctx->IAGetInputLayout(&sIl);
    D3D11_PRIMITIVE_TOPOLOGY sTop; ctx->IAGetPrimitiveTopology(&sTop);
    ID3D11ShaderResourceView* sPsrv[2] = {}; ctx->PSGetShaderResources(67, 2, sPsrv);
    ID3D11ShaderResourceView* sBg = 0; ctx->PSGetShaderResources(80, 1, &sBg);
    ID3D11BlendState* sBl = 0; FLOAT sBf[4]; UINT sBm; ctx->OMGetBlendState(&sBl, sBf, &sBm);
    ID3D11SamplerState* sSmp[3] = {}; ctx->PSGetSamplers(11, 3, sSmp);
    // ours
    ctx->VSSetConstantBuffers(10, 1, &g_ldCb);
    ctx->PSSetShaderResources(80, 1, &g_hrt[6].srv);
    ID3D11ShaderResourceView* sky[2] = { g_panoSrv[g_panoLast], g_probeSrv };
    ctx->PSSetShaderResources(67, 2, sky);
    ID3D11SamplerState* smp[3] = { g_sPoint, g_sWrap, g_sText };
    ctx->PSSetSamplers(11, 3, smp);
    ctx->IASetInputLayout(0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->OMSetBlendState(g_rfBlend, 0, 0xffffffff);
    ctx->VSSetShader(g_vsLens, 0, 0);
    oPSSetShader(ctx, g_psLens, 0, 0);
    oDrawInstanced(ctx, 4, n, 0, 0);
    // back
    ctx->VSSetConstantBuffers(10, 1, &sVcb); ctx->VSSetShader(sVs, 0, 0); ctx->IASetInputLayout(sIl); ctx->IASetPrimitiveTopology(sTop);
    ctx->PSSetShaderResources(67, 2, sPsrv); ctx->PSSetShaderResources(80, 1, &sBg); ctx->OMSetBlendState(sBl, sBf, sBm); ctx->PSSetSamplers(11, 3, sSmp);
    IUnknown* rel[] = { sVcb, sVs, sIl, sPsrv[0], sPsrv[1], sBg, sBl, sSmp[0], sSmp[1], sSmp[2] };
    for (int i = 0; i < (int)(sizeof(rel) / sizeof(rel[0])); ++i) if (rel[i]) rel[i]->Release();
}

// 1.88b: the game's headlights drawn lately (headlight.inc captured their spot): the rain lit by them (P31.y)
static bool HlOnNow() { return g_hlCb && g_hlTex && g_hlFrame + 2 >= g_frame + 1; }
