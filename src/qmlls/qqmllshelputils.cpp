// Copyright (C) 2024 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qqmllshelputils_p.h"

#include <QtQmlLS/private/qqmllsutils_p.h>
#include <QtCore/private/qfactoryloader_p.h>
#include <QtCore/qlibraryinfo.h>
#include <QtCore/qdiriterator.h>
#include <QtCore/qdir.h>
#include <QtQmlCompiler/private/qqmljstyperesolver_p.h>
#include <optional>

QT_BEGIN_NAMESPACE

Q_LOGGING_CATEGORY(QQmlLSHelpUtilsLog, "qt.languageserver.helpUtils")

using namespace QQmlJS::Dom;

static QStringList documentationFiles(const QString &qtInstallationPath)
{
    QStringList result;
    QDirIterator dirIterator(qtInstallationPath, QStringList{ "*.qch"_L1 }, QDir::Files);
    while (dirIterator.hasNext()) {
        const auto fileInfo = dirIterator.nextFileInfo();
        result << fileInfo.absoluteFilePath();
    }
    return result;
}

HelpManager::HelpManager()
{
    const QFactoryLoader pluginLoader(QQmlLSHelpPluginInterface_iid, u"/help"_s);
    const auto keys = pluginLoader.metaDataKeys();
    for (qsizetype i = 0; i < keys.size(); ++i) {
        auto instance = qobject_cast<QQmlLSHelpPluginInterface *>(pluginLoader.instance(i));
        if (instance) {
            m_helpPlugin =
                    instance->initialize(QDir::tempPath() + "/collectionFile.qhc"_L1, nullptr);
            break;
        }
    }
}

void HelpManager::setDocumentationRootPath(const QString &path)
{
    if (m_docRootPath == path)
        return;
    m_docRootPath = path;

    const auto foundQchFiles = documentationFiles(path);
    if (foundQchFiles.isEmpty()) {
        qCWarning(QQmlLSHelpUtilsLog)
                << "No documentation files found in the Qt doc installation path: " << path;
        return;
    }

    return registerDocumentations(foundQchFiles);
}

QString HelpManager::documentationRootPath() const
{
    return m_docRootPath;
}

void HelpManager::registerDocumentations(const QStringList &docs) const
{
    if (!m_helpPlugin)
        return;
    std::for_each(docs.cbegin(), docs.cend(),
                  [this](const auto &file) { m_helpPlugin->registerDocumentation(file); });
}

std::optional<QByteArray> HelpManager::extractDocumentation(const DomItem &item) const
{
    if (item.internalKind() == DomType::ScriptIdentifierExpression) {
        const auto resolvedType =
                QQmlLSUtils::resolveExpressionType(item, QQmlLSUtils::ResolveOwnerType);
        if (!resolvedType)
            return std::nullopt;

        return extractDocumentationForIdentifiers(item, resolvedType.value());
    } else {
        return extractDocumentationForDomElements(item);
    }

    Q_UNREACHABLE_RETURN(std::nullopt);
}

std::optional<QByteArray>
HelpManager::extractDocumentationForIdentifiers(const DomItem &item,
                                                QQmlLSUtils::ExpressionType expr) const
{
    const auto qmlFile = item.containingFile().as<QmlFile>();
    if (!qmlFile)
        return std::nullopt;
    const auto links = collectDocumentationLinks(expr.semanticScope, qmlFile->typeResolver(),
                                                 expr.name.value());
    switch (expr.type) {
    case QQmlLSUtils::QmlObjectIdIdentifier:
    case QQmlLSUtils::JavaScriptIdentifier:
    case QQmlLSUtils::GroupedPropertyIdentifier:
    case QQmlLSUtils::PropertyIdentifier: {
        ExtractDocumentation extractor(DomType::PropertyDefinition);
        return tryExtract(extractor, links, expr.name.value());
    }
    case QQmlLSUtils::PropertyChangedSignalIdentifier:
    case QQmlLSUtils::PropertyChangedHandlerIdentifier:
    case QQmlLSUtils::SignalIdentifier:
    case QQmlLSUtils::SignalHandlerIdentifier:
    case QQmlLSUtils::MethodIdentifier: {
        ExtractDocumentation extractor(DomType::MethodInfo);
        return tryExtract(extractor, links, expr.name.value());
    }
    case QQmlLSUtils::SingletonIdentifier:
    case QQmlLSUtils::AttachedTypeIdentifier:
    case QQmlLSUtils::QmlComponentIdentifier: {
        ExtractDocumentation extractor(DomType::QmlObject);
        return tryExtract(extractor, links, expr.name.value());
    }

    // Not implemented yet
    case QQmlLSUtils::EnumeratorIdentifier:
    case QQmlLSUtils::EnumeratorValueIdentifier:
    default:
        qCDebug(QQmlLSHelpUtilsLog)
                << "Documentation extraction for" << expr.name.value() << "was not implemented";
        return std::nullopt;
    }
    Q_UNREACHABLE_RETURN(std::nullopt);
}

std::optional<QByteArray> HelpManager::extractDocumentationForDomElements(const DomItem &item) const
{
    const auto qmlFile = item.containingFile().as<QmlFile>();
    if (!qmlFile)
        return std::nullopt;

    const auto name = item.field(Fields::name).value().toString();
    std::vector<QQmlLSHelpProviderBase::DocumentLink> links;
    switch (item.internalKind()) {
    case DomType::QmlObject: {
        links = collectDocumentationLinks(item.nearestSemanticScope(), qmlFile->typeResolver(),
                                          name);
        break;
    }
    case DomType::PropertyDefinition: {
        const auto scope =
                QQmlLSUtils::findDefiningScopeForProperty(item.nearestSemanticScope(), name);
        links = collectDocumentationLinks(scope, qmlFile->typeResolver(), name);
        break;
    }
    case DomType::Binding: {
        const auto scope =
                QQmlLSUtils::findDefiningScopeForBinding(item.nearestSemanticScope(), name);
        links = collectDocumentationLinks(scope, qmlFile->typeResolver(), name);
        break;
    }
    case DomType::MethodInfo: {
        const auto scope =
                QQmlLSUtils::findDefiningScopeForMethod(item.nearestSemanticScope(), name);
        links = collectDocumentationLinks(scope, qmlFile->typeResolver(), name);
        break;
    }
    default:
        qCDebug(QQmlLSHelpUtilsLog)
                << item.internalKindStr() << "was not implemented for documentation extraction";
        return std::nullopt;
    }

    ExtractDocumentation extractor(item.internalKind());
    return tryExtract(extractor, links, name);
}

std::optional<QByteArray>
HelpManager::tryExtract(ExtractDocumentation &extractor,
                        const std::vector<QQmlLSHelpProviderBase::DocumentLink> &links,
                        const QString &name) const
{
    if (!m_helpPlugin)
        return std::nullopt;

    for (auto &&link : links) {
        const auto fileData = m_helpPlugin->fileData(link.url);
        if (fileData.isEmpty()) {
            qCDebug(QQmlLSHelpUtilsLog) << "No documentation found for" << link.url;
            continue;
        }
        const auto &documentation = extractor.execute(QString::fromUtf8(fileData), name,
                                                      HtmlExtractor::ExtractionMode::Simplified);
        if (documentation.isEmpty())
            continue;
        return documentation.toUtf8();
    }

    return std::nullopt;
}

std::optional<QByteArray>
HelpManager::documentationForItem(const DomItem &file, QLspSpecification::Position position) const
{
    if (!m_helpPlugin)
        return std::nullopt;

    if (m_helpPlugin->registeredNamespaces().empty())
        return std::nullopt;

    std::optional<QByteArray> result;
    const auto [line, character] = position;
    const auto itemLocations = QQmlLSUtils::itemsFromTextLocation(file, line, character);

    // Process found item's internalKind and fetch its documentation.
    for (const auto &entry : itemLocations) {
        result = extractDocumentation(entry.domItem);
        if (result.has_value())
            break;
    }

    return result;
}

/*
 * Returns the list of potential documentation links for the given item.
 * A keyword is not necessarily a unique name, so we need to find the scope where
 * the keyword is defined. If the item is a property, method or binding, it will
 * search for the defining scope and return the documentation links by looking at
 * the imported names. If the item is a QmlObject, it will return the documentation
 * links for qmlobject name.
 */
std::vector<QQmlLSHelpProviderBase::DocumentLink>
HelpManager::collectDocumentationLinks(QQmlJSScope::ConstPtr scope,
                                       std::shared_ptr<QQmlJSTypeResolver> typeResolver,
                                       const QString &name) const
{
    if (!m_helpPlugin)
        return {};
    const auto potentialDocumentationLinks =
            [this](QQmlJSScope::ConstPtr scope, std::shared_ptr<QQmlJSTypeResolver> typeResolver)
            -> std::vector<QQmlLSHelpProviderBase::DocumentLink> {
        if (!scope || !typeResolver)
            return {};

        std::vector<QQmlLSHelpProviderBase::DocumentLink> links;
        const auto docLinks = m_helpPlugin->documentsForKeyword(typeResolver->nameForType(scope));
        std::copy(docLinks.cbegin(), docLinks.cend(), std::back_inserter(links));
        return links;
    };

    // If the scope is not found for the defined scope, return all the links related to this name.
    const auto result = potentialDocumentationLinks(scope, typeResolver);
    return result.empty() ? m_helpPlugin->documentsForKeyword(name) : result;
}

QT_END_NAMESPACE
