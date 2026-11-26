#include "generator_mupdf.h"

#include <okular/core/action.h>
#include <okular/core/area.h>
#include <okular/core/fileprinter.h>
#include <okular/core/page.h>
#include <okular/core/textpage.h>

#include <KPluginFactory>

#include <QDateTime>
#include <QDomElement>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLocale>
#include <QLoggingCategory>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>

K_PLUGIN_CLASS_WITH_JSON(MupdfGenerator, "okularGenerator_mupdf.json")

Q_LOGGING_CATEGORY(lcMupdf, "okular.mupdf", QtWarningMsg)

namespace
{
constexpr size_t kStoreBytes = size_t(2) << 30;
constexpr size_t kDisplayListCacheSize = 32;
constexpr int kMinBandHeight = 64;
constexpr auto kAbortPollInterval = std::chrono::milliseconds(5);
constexpr int kLabelBufferSize = 256;
constexpr int kMetadataBufferSize = 4096;
constexpr float kFallbackPageWidth = 612;
constexpr float kFallbackPageHeight = 792;

struct MetadataField {
    Okular::DocumentInfo::Key key;
    const char *fitzKey;
    bool isDate;
};

const MetadataField kMetadataFields[] = {
    {Okular::DocumentInfo::Title, FZ_META_INFO_TITLE, false},
    {Okular::DocumentInfo::Author, FZ_META_INFO_AUTHOR, false},
    {Okular::DocumentInfo::Subject, FZ_META_INFO_SUBJECT, false},
    {Okular::DocumentInfo::Keywords, FZ_META_INFO_KEYWORDS, false},
    {Okular::DocumentInfo::Creator, FZ_META_INFO_CREATOR, false},
    {Okular::DocumentInfo::Producer, FZ_META_INFO_PRODUCER, false},
    {Okular::DocumentInfo::CreationDate, FZ_META_INFO_CREATIONDATE, true},
    {Okular::DocumentInfo::ModificationDate, FZ_META_INFO_MODIFICATIONDATE, true},
};

double rectWidth(const fz_rect &rect)
{
    return rect.x1 - rect.x0;
}

double rectHeight(const fz_rect &rect)
{
    return rect.y1 - rect.y0;
}

fz_rect usableBounds(fz_rect bounds)
{
    if (fz_is_empty_rect(bounds)) {
        return fz_make_rect(0, 0, kFallbackPageWidth, kFallbackPageHeight);
    }
    return bounds;
}

Okular::NormalizedRect normalizedIn(const fz_rect &rect, const fz_rect &bounds)
{
    const double width = rectWidth(bounds);
    const double height = rectHeight(bounds);
    return Okular::NormalizedRect((rect.x0 - bounds.x0) / width, (rect.y0 - bounds.y0) / height, (rect.x1 - bounds.x0) / width, (rect.y1 - bounds.y0) / height);
}

QString formatPdfDate(const QString &raw)
{
    const QString digits = raw.startsWith(QLatin1String("D:")) ? raw.mid(2) : raw;
    const QDateTime moment = QDateTime::fromString(digits.left(14), QStringLiteral("yyyyMMddHHmmss"));
    if (!moment.isValid()) {
        return raw;
    }
    return QLocale().toString(moment, QLocale::LongFormat);
}

QString codepointText(int codepoint)
{
    const char32_t character = char32_t(codepoint);
    return QString::fromUcs4(&character, 1);
}

void appendBlocks(Okular::TextPage &textPage, fz_stext_block *block, const fz_rect &bounds);

void appendLine(Okular::TextPage &textPage, fz_stext_line *line, const fz_rect &bounds)
{
    for (fz_stext_char *character = line->first_char; character; character = character->next) {
        QString glyph = codepointText(character->c);
        if (!character->next) {
            glyph.append(QLatin1Char('\n'));
        }
        textPage.append(glyph, normalizedIn(fz_rect_from_quad(character->quad), bounds));
    }
}

void appendBlocks(Okular::TextPage &textPage, fz_stext_block *block, const fz_rect &bounds)
{
    for (; block; block = block->next) {
        if (block->type == FZ_STEXT_BLOCK_TEXT) {
            for (fz_stext_line *line = block->u.t.first_line; line; line = line->next) {
                appendLine(textPage, line, bounds);
            }
        } else if (block->type == FZ_STEXT_BLOCK_STRUCT && block->u.s.down) {
            appendBlocks(textPage, block->u.s.down->first_block, bounds);
        }
    }
}
}

MupdfGenerator::MupdfGenerator(QObject *parent, const QVariantList &args)
    : Okular::Generator(parent, args)
{
    setFeature(Threaded);
    setFeature(TextExtraction);
    setFeature(TiledRendering);
    setFeature(SupportsCancelling);
    setFeature(PrintPostscript);

    const fz_locks_context locks = m_locks.descriptor();
    m_rootContext.reset(fz_new_context(nullptr, &locks, kStoreBytes));
    if (m_rootContext && !registerHandlers(m_rootContext.get())) {
        m_rootContext.reset();
    }
}

MupdfGenerator::~MupdfGenerator()
{
    QMutexLocker locker(&m_documentMutex);
    releaseDocument();
}

ContextPtr MupdfGenerator::cloneContext() const
{
    if (!m_rootContext) {
        return ContextPtr();
    }
    return ContextPtr(fz_clone_context(m_rootContext.get()));
}

bool MupdfGenerator::loadDocument(const QString &fileName, QList<Okular::Page *> &pages)
{
    return loadDocumentWithPassword(fileName, pages, QString()) == Okular::Document::OpenSuccess;
}

Okular::Document::OpenResult MupdfGenerator::loadDocumentWithPassword(const QString &fileName, QList<Okular::Page *> &pages, const QString &password)
{
    QMutexLocker locker(&m_documentMutex);
    releaseDocument();
    qCDebug(lcMupdf) << "open" << fileName;

    ContextPtr ctx = cloneContext();
    if (!ctx) {
        return Okular::Document::OpenError;
    }

    const QByteArray path = QFile::encodeName(fileName);
    m_document = openDocument(ctx.get(), path.constData());
    if (!m_document) {
        return Okular::Document::OpenError;
    }

    const std::optional<bool> locked = needsPassword(ctx.get(), m_document);
    if (!locked) {
        releaseDocument();
        return Okular::Document::OpenError;
    }
    if (*locked && !authenticate(ctx.get(), m_document, password.toUtf8().constData())) {
        releaseDocument();
        return Okular::Document::OpenNeedsPassword;
    }

    const int count = countPages(ctx.get(), m_document);
    if (count <= 0) {
        releaseDocument();
        return Okular::Document::OpenError;
    }

    m_filePath = fileName;
    m_pageBounds.assign(count, fz_empty_rect);
    m_displayLists.assign(count, nullptr);
    m_linksLoaded.assign(count, false);
    pages.resize(count);
    for (int number = 0; number < count; ++number) {
        pages[number] = describePage(ctx.get(), number);
    }
    return Okular::Document::OpenSuccess;
}

Okular::Page *MupdfGenerator::describePage(fz_context *ctx, int number)
{
    char label[kLabelBufferSize] = {};
    fz_rect bounds = fz_empty_rect;
    fz_page *page = loadPage(ctx, m_document, number);
    if (page) {
        bounds = boundPage(ctx, page);
        pageLabel(ctx, page, label, kLabelBufferSize);
        fz_drop_page(ctx, page);
    }
    m_pageBounds[number] = usableBounds(bounds);

    auto *okularPage = new Okular::Page(number, rectWidth(m_pageBounds[number]), rectHeight(m_pageBounds[number]), Okular::Rotation0);
    if (label[0]) {
        okularPage->setLabel(QString::fromUtf8(label));
    }
    return okularPage;
}

bool MupdfGenerator::doCloseDocument()
{
    QMutexLocker locker(&m_documentMutex);
    releaseDocument();
    return true;
}

void MupdfGenerator::releaseDocument()
{
    ContextPtr ctx = cloneContext();
    if (ctx) {
        for (fz_display_list *list : m_displayLists) {
            fz_drop_display_list(ctx.get(), list);
        }
        fz_drop_document(ctx.get(), m_document);
    }
    m_document = nullptr;
    m_displayLists.clear();
    m_linksLoaded.clear();
    m_recentPages.clear();
    m_pageBounds.clear();
    m_synopsis.reset();
    m_filePath.clear();
}

fz_display_list *MupdfGenerator::acquireDisplayList(fz_context *ctx, Okular::Page *page)
{
    QMutexLocker locker(&m_documentMutex);
    const int number = page->number();
    if (!m_document || number < 0 || number >= int(m_displayLists.size())) {
        return nullptr;
    }

    if (!m_displayLists[number]) {
        fz_page *fitzPage = loadPage(ctx, m_document, number);
        if (!fitzPage) {
            return nullptr;
        }
        m_displayLists[number] = recordPage(ctx, fitzPage);
        if (!m_linksLoaded[number]) {
            page->setObjectRects(pageLinks(ctx, fitzPage, number));
            m_linksLoaded[number] = true;
        }
        fz_drop_page(ctx, fitzPage);
        if (!m_displayLists[number]) {
            return nullptr;
        }
    }

    markRecentlyUsed(ctx, number);
    return fz_keep_display_list(ctx, m_displayLists[number]);
}

void MupdfGenerator::markRecentlyUsed(fz_context *ctx, int number)
{
    m_recentPages.remove(number);
    m_recentPages.push_front(number);
    while (m_recentPages.size() > kDisplayListCacheSize) {
        const int evicted = m_recentPages.back();
        m_recentPages.pop_back();
        fz_drop_display_list(ctx, m_displayLists[evicted]);
        m_displayLists[evicted] = nullptr;
    }
}

QList<Okular::ObjectRect *> MupdfGenerator::pageLinks(fz_context *ctx, fz_page *page, int number) const
{
    QList<Okular::ObjectRect *> rects;
    fz_link *links = loadLinks(ctx, page);
    for (fz_link *link = links; link; link = link->next) {
        if (!link->uri) {
            continue;
        }
        Okular::Action *action = linkAction(ctx, link->uri);
        if (action) {
            rects.append(new Okular::ObjectRect(normalizedIn(link->rect, m_pageBounds[number]), false, Okular::ObjectRect::Action, action));
        }
    }
    fz_drop_link(ctx, links);
    return rects;
}

Okular::Action *MupdfGenerator::linkAction(fz_context *ctx, const char *uri) const
{
    if (fz_is_external_link(ctx, uri)) {
        return new Okular::BrowseAction(QUrl(QString::fromUtf8(uri)));
    }
    const std::optional<fz_link_dest> destination = resolveLinkDest(ctx, m_document, uri);
    if (!destination || destination->loc.page < 0) {
        return nullptr;
    }
    const int target = pageNumberFromLocation(ctx, m_document, destination->loc);
    if (target < 0 || target >= int(m_pageBounds.size())) {
        return nullptr;
    }
    return new Okular::GotoAction(QString(), viewportAt(target, destination->x, destination->y));
}

Okular::DocumentViewport MupdfGenerator::viewportAt(int number, float x, float y) const
{
    Okular::DocumentViewport viewport(number);
    if (std::isnan(x) || std::isnan(y)) {
        return viewport;
    }
    const fz_rect &bounds = m_pageBounds[number];
    viewport.rePos.enabled = true;
    viewport.rePos.normalizedX = std::clamp((x - bounds.x0) / rectWidth(bounds), 0.0, 1.0);
    viewport.rePos.normalizedY = std::clamp((y - bounds.y0) / rectHeight(bounds), 0.0, 1.0);
    viewport.rePos.pos = Okular::DocumentViewport::TopLeft;
    return viewport;
}

QImage MupdfGenerator::image(Okular::PixmapRequest *request)
{
    if (request->shouldAbortRender()) {
        return QImage();
    }

    QElapsedTimer timer;
    timer.start();
    Okular::Page *page = request->page();
    const QRect target = request->isTile() ? request->normalizedRect().geometry(request->width(), request->height()) : QRect(0, 0, request->width(), request->height());
    QImage result(target.size(), QImage::Format_RGB32);
    if (result.isNull()) {
        return QImage();
    }

    ContextPtr ctx = cloneContext();
    fz_display_list *list = ctx ? acquireDisplayList(ctx.get(), page) : nullptr;
    if (!list) {
        result.fill(Qt::white);
        return result;
    }

    const fz_rect bounds = m_pageBounds[page->number()];
    const fz_matrix ctm = fz_concat(fz_translate(-bounds.x0, -bounds.y0), fz_scale(request->width() / rectWidth(bounds), request->height() / rectHeight(bounds)));
    const bool completed = renderParallel(list, ctm, target, result, request);
    fz_drop_display_list(ctx.get(), list);
    qCDebug(lcMupdf).nospace() << "page " << page->number() << (request->isTile() ? " tile " : " full ") << target << (completed ? " done " : " aborted ") << timer.elapsed() << " ms";
    return completed ? result : QImage();
}

bool MupdfGenerator::renderParallel(fz_display_list *list, fz_matrix ctm, const QRect &target, QImage &image, Okular::PixmapRequest *request) const
{
    const int bandCount = std::clamp(target.height() / kMinBandHeight, 1, std::max(1, QThread::idealThreadCount()));
    const int bandHeight = (target.height() + bandCount - 1) / bandCount;
    std::vector<fz_cookie> cookies(bandCount);
    std::vector<std::future<void>> bands;
    bands.reserve(bandCount);

    for (int index = 0; index < bandCount; ++index) {
        const int top = index * bandHeight;
        const int bottom = std::min(top + bandHeight, target.height());
        if (top >= bottom) {
            break;
        }
        const fz_irect area{target.x(), target.y() + top, target.x() + target.width(), target.y() + bottom};
        unsigned char *samples = image.scanLine(top);
        fz_cookie *cookie = &cookies[index];
        bands.push_back(std::async(std::launch::async, [this, list, ctm, area, samples, cookie] {
            ContextPtr ctx = cloneContext();
            if (ctx) {
                renderBand(ctx.get(), list, ctm, area, samples, cookie);
            }
        }));
    }

    bool aborted = false;
    for (std::future<void> &band : bands) {
        while (band.wait_for(kAbortPollInterval) == std::future_status::timeout) {
            if (!aborted && request->shouldAbortRender()) {
                aborted = true;
                for (fz_cookie &cookie : cookies) {
                    cookie.abort = 1;
                }
            }
        }
    }
    return !aborted && !request->shouldAbortRender();
}

Okular::TextPage *MupdfGenerator::textPage(Okular::TextRequest *request)
{
    auto *result = new Okular::TextPage;
    Okular::Page *page = request->page();
    ContextPtr ctx = cloneContext();
    fz_display_list *list = ctx ? acquireDisplayList(ctx.get(), page) : nullptr;
    if (!list) {
        return result;
    }

    fz_stext_page *text = extractText(ctx.get(), list);
    fz_drop_display_list(ctx.get(), list);
    if (text) {
        appendBlocks(*result, text->first_block, m_pageBounds[page->number()]);
        fz_drop_stext_page(ctx.get(), text);
    }
    return result;
}

const Okular::DocumentSynopsis *MupdfGenerator::generateDocumentSynopsis()
{
    QMutexLocker locker(&m_documentMutex);
    if (m_synopsis) {
        return m_synopsis.get();
    }

    ContextPtr ctx = cloneContext();
    fz_outline *outline = (ctx && m_document) ? loadOutline(ctx.get(), m_document) : nullptr;
    if (!outline) {
        return nullptr;
    }

    m_synopsis = std::make_unique<Okular::DocumentSynopsis>();
    appendOutline(ctx.get(), outline, *m_synopsis);
    fz_drop_outline(ctx.get(), outline);
    return m_synopsis.get();
}

void MupdfGenerator::appendOutline(fz_context *ctx, fz_outline *item, QDomNode &parent)
{
    for (; item; item = item->next) {
        QDomElement entry = m_synopsis->createElement(QString::fromUtf8(item->title ? item->title : ""));
        parent.appendChild(entry);

        if (item->uri && fz_is_external_link(ctx, item->uri)) {
            entry.setAttribute(QStringLiteral("URL"), QString::fromUtf8(item->uri));
        } else if (item->page.page >= 0) {
            const int target = pageNumberFromLocation(ctx, m_document, item->page);
            if (target >= 0 && target < int(m_pageBounds.size())) {
                entry.setAttribute(QStringLiteral("Viewport"), viewportAt(target, item->x, item->y).toString());
            }
        }
        entry.setAttribute(QStringLiteral("Open"), item->is_open ? QStringLiteral("true") : QStringLiteral("false"));

        if (item->down) {
            appendOutline(ctx, item->down, entry);
        }
    }
}

QString MupdfGenerator::metadata(fz_context *ctx, const char *key) const
{
    char buffer[kMetadataBufferSize] = {};
    if (!lookupMetadata(ctx, m_document, key, buffer, kMetadataBufferSize)) {
        return QString();
    }
    return QString::fromUtf8(buffer);
}

Okular::DocumentInfo MupdfGenerator::generateDocumentInfo(const QSet<Okular::DocumentInfo::Key> &keys) const
{
    Okular::DocumentInfo info;
    info.set(Okular::DocumentInfo::MimeType, QStringLiteral("application/pdf"));

    QMutexLocker locker(&m_documentMutex);
    if (!m_document) {
        return info;
    }
    info.set(Okular::DocumentInfo::Pages, QString::number(m_pageBounds.size()));

    ContextPtr ctx = cloneContext();
    if (!ctx) {
        return info;
    }
    for (const MetadataField &field : kMetadataFields) {
        if (!keys.contains(field.key)) {
            continue;
        }
        const QString value = metadata(ctx.get(), field.fitzKey);
        if (!value.isEmpty()) {
            info.set(field.key, field.isDate ? formatPdfDate(value) : value);
        }
    }
    return info;
}

Okular::Document::PrintError MupdfGenerator::print(QPrinter &printer)
{
    return Okular::FilePrinter::printFile(printer,
                                          m_filePath,
                                          document()->orientation(),
                                          Okular::FilePrinter::ApplicationDeletesFiles,
                                          Okular::FilePrinter::SystemSelectsPages,
                                          document()->bookmarkedPageRange());
}

#include "generator_mupdf.moc"
