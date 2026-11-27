// First-page PDF thumbnails using the Windows 10/11 renderer and Explorer's read-only stream.
#include "Common.h"
#include <shcore.h>
#include <roapi.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <cmath>
#include <memory>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "shcore.lib")

namespace {
using namespace winrt;
using namespace winrt::Windows::Data::Pdf;
using namespace winrt::Windows::Storage::Streams;

// The thumbnail provider can be called on an STA. Dispatch COM calls while the native
// PDF renderer reads the supplied stream, including apartment-bound IStream implementations.
template<class Operation>
auto Await(Operation const& op)
{
    auto done = std::make_shared<handle>(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!*done) throw_last_error();
    op.Completed([done](auto const&, auto) noexcept { SetEvent(done->get()); });
    HANDLE wait = done->get();
    DWORD index = 0;
    check_hresult(CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS, INFINITE, 1, &wait, &index));
    return op.GetResults();
}

bool RenderFirstPage(IStream* source, UINT maxSize, Image& out)
{
    IRandomAccessStream input{nullptr};
    check_hresult(CreateRandomAccessStreamOverStream(source, BSOS_DEFAULT,
        winrt::guid_of<IRandomAccessStream>(), winrt::put_abi(input)));
    auto doc = Await(PdfDocument::LoadFromStreamAsync(input));
    if (!doc.PageCount()) return false;
    auto page = doc.GetPage(0);
    const auto size = page.Size();
    if (!std::isfinite(size.Width) || !std::isfinite(size.Height) || std::min(size.Width, size.Height) <= 0)
        return false;
    const double scale = maxSize / double(std::max(size.Width, size.Height));
    PdfPageRenderOptions options;
    options.DestinationWidth(std::max(1u, UINT(std::lround(size.Width * scale))));
    options.DestinationHeight(std::max(1u, UINT(std::lround(size.Height * scale))));
    InMemoryRandomAccessStream output;
    Await(page.RenderToStreamAsync(output, options));
    page.Close();
    output.Seek(0);
    com_ptr<IStream> result;
    check_hresult(CreateStreamOverRandomAccessStream(output.as<::IUnknown>().get(), IID_PPV_ARGS(result.put())));
    return DecodeWithWic(result.get(), out);
}
}

bool LoadPdf(IStream* stream, UINT maxSize, Image& out)
{
    HRESULT init = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return false;
    struct ApartmentScope {
        HRESULT init;
        ~ApartmentScope() { if (SUCCEEDED(init)) RoUninitialize(); }
    } apartment{init};
    try { return RenderFirstPage(stream, maxSize, out); }
    catch (const winrt::hresult_error&) { return false; } // includes password-protected PDFs
}

