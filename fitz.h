#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <optional>

#include <mupdf/fitz.h>

class FitzLocks
{
public:
    fz_locks_context descriptor();

private:
    static void lock(void *user, int index);
    static void unlock(void *user, int index);

    std::array<std::mutex, FZ_LOCK_MAX> m_mutexes;
};

struct ContextDropper {
    void operator()(fz_context *ctx) const;
};

using ContextPtr = std::unique_ptr<fz_context, ContextDropper>;

template<typename Result, typename Producer>
Result fitzProduce(fz_context *ctx, Result fallback, Producer &&producer)
{
    Result result = fallback;
    fz_var(result);
    fz_try(ctx)
    {
        result = producer();
    }
    fz_catch(ctx)
    {
        fz_report_error(ctx);
    }
    return result;
}

bool registerHandlers(fz_context *ctx);
fz_document *openDocument(fz_context *ctx, const char *path);
std::optional<bool> needsPassword(fz_context *ctx, fz_document *doc);
bool authenticate(fz_context *ctx, fz_document *doc, const char *password);
int countPages(fz_context *ctx, fz_document *doc);
fz_page *loadPage(fz_context *ctx, fz_document *doc, int number);
fz_rect boundPage(fz_context *ctx, fz_page *page);
bool pageLabel(fz_context *ctx, fz_page *page, char *buffer, int size);
fz_display_list *recordPage(fz_context *ctx, fz_page *page);
fz_link *loadLinks(fz_context *ctx, fz_page *page);
std::optional<fz_link_dest> resolveLinkDest(fz_context *ctx, fz_document *doc, const char *uri);
int pageNumberFromLocation(fz_context *ctx, fz_document *doc, fz_location location);
fz_outline *loadOutline(fz_context *ctx, fz_document *doc);
bool lookupMetadata(fz_context *ctx, fz_document *doc, const char *key, char *buffer, int size);
fz_stext_page *extractText(fz_context *ctx, fz_display_list *list);
bool renderBand(fz_context *ctx, fz_display_list *list, fz_matrix ctm, fz_irect band, unsigned char *samples, fz_cookie *cookie);
