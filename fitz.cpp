#include "fitz.h"

fz_locks_context FitzLocks::descriptor()
{
    return fz_locks_context{this, &FitzLocks::lock, &FitzLocks::unlock};
}

void FitzLocks::lock(void *user, int index)
{
    static_cast<FitzLocks *>(user)->m_mutexes[index].lock();
}

void FitzLocks::unlock(void *user, int index)
{
    static_cast<FitzLocks *>(user)->m_mutexes[index].unlock();
}

void ContextDropper::operator()(fz_context *ctx) const
{
    fz_drop_context(ctx);
}

bool registerHandlers(fz_context *ctx)
{
    return fitzProduce(ctx, false, [ctx] {
        fz_register_document_handlers(ctx);
        return true;
    });
}

fz_document *openDocument(fz_context *ctx, const char *path)
{
    return fitzProduce<fz_document *>(ctx, nullptr, [ctx, path] {
        return fz_open_document(ctx, path);
    });
}

std::optional<bool> needsPassword(fz_context *ctx, fz_document *doc)
{
    return fitzProduce<std::optional<bool>>(ctx, std::nullopt, [ctx, doc] {
        return std::optional<bool>(fz_needs_password(ctx, doc) != 0);
    });
}

bool authenticate(fz_context *ctx, fz_document *doc, const char *password)
{
    return fitzProduce(ctx, false, [ctx, doc, password] {
        return fz_authenticate_password(ctx, doc, password) != 0;
    });
}

int countPages(fz_context *ctx, fz_document *doc)
{
    return fitzProduce(ctx, -1, [ctx, doc] {
        return fz_count_pages(ctx, doc);
    });
}

fz_page *loadPage(fz_context *ctx, fz_document *doc, int number)
{
    return fitzProduce<fz_page *>(ctx, nullptr, [ctx, doc, number] {
        return fz_load_page(ctx, doc, number);
    });
}

fz_rect boundPage(fz_context *ctx, fz_page *page)
{
    return fitzProduce(ctx, fz_empty_rect, [ctx, page] {
        return fz_bound_page(ctx, page);
    });
}

bool pageLabel(fz_context *ctx, fz_page *page, char *buffer, int size)
{
    return fitzProduce(ctx, false, [ctx, page, buffer, size] {
        fz_page_label(ctx, page, buffer, size);
        return true;
    });
}

fz_display_list *recordPage(fz_context *ctx, fz_page *page)
{
    return fitzProduce<fz_display_list *>(ctx, nullptr, [ctx, page] {
        return fz_new_display_list_from_page(ctx, page);
    });
}

fz_link *loadLinks(fz_context *ctx, fz_page *page)
{
    return fitzProduce<fz_link *>(ctx, nullptr, [ctx, page] {
        return fz_load_links(ctx, page);
    });
}

std::optional<fz_link_dest> resolveLinkDest(fz_context *ctx, fz_document *doc, const char *uri)
{
    return fitzProduce<std::optional<fz_link_dest>>(ctx, std::nullopt, [ctx, doc, uri] {
        return std::optional<fz_link_dest>(fz_resolve_link_dest(ctx, doc, uri));
    });
}

int pageNumberFromLocation(fz_context *ctx, fz_document *doc, fz_location location)
{
    return fitzProduce(ctx, -1, [ctx, doc, location] {
        return fz_page_number_from_location(ctx, doc, location);
    });
}

fz_outline *loadOutline(fz_context *ctx, fz_document *doc)
{
    return fitzProduce<fz_outline *>(ctx, nullptr, [ctx, doc] {
        return fz_load_outline(ctx, doc);
    });
}

bool lookupMetadata(fz_context *ctx, fz_document *doc, const char *key, char *buffer, int size)
{
    return fitzProduce(ctx, false, [ctx, doc, key, buffer, size] {
        return fz_lookup_metadata(ctx, doc, key, buffer, size) > 0;
    });
}

fz_stext_page *extractText(fz_context *ctx, fz_display_list *list)
{
    return fitzProduce<fz_stext_page *>(ctx, nullptr, [ctx, list] {
        const fz_stext_options options{};
        return fz_new_stext_page_from_display_list(ctx, list, &options);
    });
}

bool renderBand(fz_context *ctx, fz_display_list *list, fz_matrix ctm, fz_irect band, unsigned char *samples, fz_cookie *cookie)
{
    fz_pixmap *pixmap = nullptr;
    fz_device *device = nullptr;
    fz_var(pixmap);
    fz_var(device);
    fz_try(ctx)
    {
        pixmap = fz_new_pixmap_with_bbox_and_data(ctx, fz_device_bgr(ctx), band, nullptr, 1, samples);
        fz_clear_pixmap_with_value(ctx, pixmap, 0xff);
        device = fz_new_draw_device(ctx, fz_identity, pixmap);
        fz_run_display_list(ctx, list, device, ctm, fz_rect_from_irect(band), cookie);
        fz_close_device(ctx, device);
    }
    fz_always(ctx)
    {
        fz_drop_device(ctx, device);
        fz_drop_pixmap(ctx, pixmap);
    }
    fz_catch(ctx)
    {
        if (cookie->abort) {
            fz_ignore_error(ctx);
        } else {
            fz_report_error(ctx);
        }
        return false;
    }
    return true;
}
