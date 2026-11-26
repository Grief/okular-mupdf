#pragma once

#include "fitz.h"

#include <okular/core/document.h>
#include <okular/core/generator.h>

#include <QMutex>

#include <list>
#include <memory>
#include <vector>

class QDomNode;
class QRect;

class MupdfGenerator : public Okular::Generator
{
    Q_OBJECT
    Q_INTERFACES(Okular::Generator)

public:
    MupdfGenerator(QObject *parent, const QVariantList &args);
    ~MupdfGenerator() override;

    bool loadDocument(const QString &fileName, QList<Okular::Page *> &pages) override;
    Okular::Document::OpenResult loadDocumentWithPassword(const QString &fileName, QList<Okular::Page *> &pages, const QString &password) override;
    Okular::DocumentInfo generateDocumentInfo(const QSet<Okular::DocumentInfo::Key> &keys) const override;
    const Okular::DocumentSynopsis *generateDocumentSynopsis() override;
    Okular::Document::PrintError print(QPrinter &printer) override;

protected:
    bool doCloseDocument() override;
    QImage image(Okular::PixmapRequest *request) override;
    Okular::TextPage *textPage(Okular::TextRequest *request) override;

private:
    ContextPtr cloneContext() const;
    void releaseDocument();
    Okular::Page *describePage(fz_context *ctx, int number);
    fz_display_list *acquireDisplayList(fz_context *ctx, Okular::Page *page);
    void markRecentlyUsed(fz_context *ctx, int number);
    QList<Okular::ObjectRect *> pageLinks(fz_context *ctx, fz_page *page, int number) const;
    Okular::Action *linkAction(fz_context *ctx, const char *uri) const;
    Okular::DocumentViewport viewportAt(int number, float x, float y) const;
    void appendOutline(fz_context *ctx, fz_outline *item, QDomNode &parent);
    QString metadata(fz_context *ctx, const char *key) const;
    bool renderParallel(fz_display_list *list, fz_matrix ctm, const QRect &target, QImage &image, Okular::PixmapRequest *request) const;

    FitzLocks m_locks;
    ContextPtr m_rootContext;
    mutable QMutex m_documentMutex;
    fz_document *m_document = nullptr;
    QString m_filePath;
    std::vector<fz_rect> m_pageBounds;
    std::vector<fz_display_list *> m_displayLists;
    std::vector<bool> m_linksLoaded;
    std::list<int> m_recentPages;
    std::unique_ptr<Okular::DocumentSynopsis> m_synopsis;
};
