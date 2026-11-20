/* dispcap: display-side frame pacing measurement (docs/hfr.md section 7).
 *
 * usage: dispcap.exe <seconds> [window title]
 *
 * Desktop Duplication on the primary output: every desktop frame the
 * compositor presents (LastPresentTime, QPC) and the present id the game
 * stamped into its top-left corner (SMB2_PACE_MARK=1: 18 blocks of 8x8
 * pixels: white, black, then 16 bits of the present id, MSB first, white =
 * 1).  Only that 144x8 pixel strip is copied.  Output lines:
 *     [dc] <qpc> <qpc freq> <accumulated frames> <id or -1>
 * The window's client origin is found by title (default: fullscreen, 0,0).
 * Works for any renderer (GL has no frame statistics).  Note: duplication
 * may keep the desktop compositor in the path of a fullscreen game. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>

#define MW 144
#define MH 8

int main(int argc, char** argv) {
    double secs = argc > 1 ? atof(argv[1]) : 10.0;
    const char* title = argc > 2 ? argv[2] : NULL;
    ID3D11Device* dev = NULL; ID3D11DeviceContext* ctx = NULL;
    IDXGIDevice* xd = NULL; IDXGIAdapter* ad = NULL; IDXGIOutput* out = NULL; IDXGIOutput1* out1 = NULL;
    IDXGIOutputDuplication* dup = NULL;
    ID3D11Texture2D* stage = NULL;
    D3D11_TEXTURE2D_DESC td;
    DXGI_OUTPUT_DESC od;
    LARGE_INTEGER f, t0, now;
    POINT org = { 0, 0 };
    HRESULT hr;
    D3D_FEATURE_LEVEL fl;
    long n = 0;
    QueryPerformanceFrequency(&f);
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) { printf("[dc] error device 0x%08lx\n", (unsigned long)hr); return 1; }
    ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void**)&xd);
    IDXGIDevice_GetAdapter(xd, &ad);
    if (FAILED(IDXGIAdapter_EnumOutputs(ad, 0, &out))) { printf("[dc] error output\n"); return 1; }
    IDXGIOutput_GetDesc(out, &od);
    IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput1, (void**)&out1);
    hr = IDXGIOutput1_DuplicateOutput(out1, (IUnknown*)dev, &dup);
    if (FAILED(hr)) { printf("[dc] error duplicate 0x%08lx\n", (unsigned long)hr); return 1; }
    memset(&td, 0, sizeof td);
    td.Width = MW; td.Height = MH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &stage);
    QueryPerformanceCounter(&t0);
    for (;;) {
        DXGI_OUTDUPL_FRAME_INFO fi;
        IDXGIResource* res = NULL;
        QueryPerformanceCounter(&now);
        if ((double)(now.QuadPart - t0.QuadPart) / (double)f.QuadPart > secs) break;
        if (title && (n % 64) == 0) {   /* the window may move or appear late */
            HWND w = FindWindowA(NULL, title);
            if (w) { org.x = 0; org.y = 0; ClientToScreen(w, &org);
                     org.x -= od.DesktopCoordinates.left; org.y -= od.DesktopCoordinates.top; }
        }
        hr = IDXGIOutputDuplication_AcquireNextFrame(dup, 100, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) {   /* access lost (mode change, secure desktop): start over */
            printf("[dc] lost 0x%08lx\n", (unsigned long)hr);
            IDXGIOutputDuplication_Release(dup); dup = NULL;
            Sleep(100);
            if (FAILED(IDXGIOutput1_DuplicateOutput(out1, (IUnknown*)dev, &dup))) { Sleep(500); continue; }
            continue;
        }
        n++;
        if (fi.LastPresentTime.QuadPart) {
            ID3D11Texture2D* tex = NULL;
            int id = -1;
            if (SUCCEEDED(IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void**)&tex))) {
                D3D11_BOX box;
                D3D11_MAPPED_SUBRESOURCE m;
                box.left = (UINT)org.x; box.top = (UINT)org.y; box.right = box.left + MW; box.bottom = box.top + MH;
                box.front = 0; box.back = 1;
                ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource*)stage, 0, 0, 0, 0, (ID3D11Resource*)tex, 0, &box);
                if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource*)stage, 0, D3D11_MAP_READ, 0, &m))) {
                    const unsigned char* row = (const unsigned char*)m.pData + 4 * m.RowPitch;
                    int b, v = 0;
                    /* sync: block 0 white, block 1 black */
                    if (row[4 * 4 + 1] > 160 && row[12 * 4 + 1] < 96) {
                        for (b = 0; b < 16; b++) v = (v << 1) | (row[(8 * (b + 2) + 4) * 4 + 1] > 128);
                        id = v;
                    }
                    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource*)stage, 0);
                }
                ID3D11Texture2D_Release(tex);
            }
            printf("[dc] %lld %lld %u %d\n", (long long)fi.LastPresentTime.QuadPart, (long long)f.QuadPart,
                   fi.AccumulatedFrames, id);
        }
        IDXGIResource_Release(res);
        IDXGIOutputDuplication_ReleaseFrame(dup);
    }
    fflush(stdout);
    return 0;
}
