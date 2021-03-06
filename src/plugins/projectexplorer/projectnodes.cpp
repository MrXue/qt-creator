/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "projectnodes.h"

#include "nodesvisitor.h"
#include "projectexplorerconstants.h"
#include "projecttree.h"

#include <coreplugin/fileiconprovider.h>
#include <coreplugin/icore.h>
#include <coreplugin/iversioncontrol.h>
#include <coreplugin/vcsmanager.h>
#include <utils/algorithm.h>
#include <utils/fileutils.h>
#include <utils/qtcassert.h>

#include <QFileInfo>
#include <QDir>
#include <QIcon>
#include <QStyle>

namespace ProjectExplorer {

/*!
  \class ProjectExplorer::Node

  \brief The Node class is the base class of all nodes in the node hierarchy.

  The nodes are arranged in a tree where leaves are FileNodes and non-leaves are FolderNodes
  A Project is a special Folder that manages the files and normal folders underneath it.

  The Watcher emits signals for structural changes in the hierarchy.
  A Visitor can be used to traverse all Projects and other Folders.

  \sa ProjectExplorer::FileNode, ProjectExplorer::FolderNode, ProjectExplorer::ProjectNode
  \sa ProjectExplorer::NodesWatcher, ProjectExplorer::NodesVisitor
*/

Node::Node(NodeType nodeType, const Utils::FileName &filePath, int line) :
    m_filePath(filePath), m_line(line), m_nodeType(nodeType)
{ }

void Node::setPriority(int p)
{
    m_priority = p;
}

void Node::emitNodeSortKeyAboutToChange()
{
    if (parentFolderNode())
        ProjectTree::instance()->emitNodeSortKeyAboutToChange(this);
}

void Node::emitNodeSortKeyChanged()
{
    if (parentFolderNode())
        ProjectTree::instance()->emitNodeSortKeyChanged(this);
}

void Node::setAbsoluteFilePathAndLine(const Utils::FileName &path, int line)
{
    if (m_filePath == path && m_line == line)
        return;

    emitNodeSortKeyAboutToChange();
    m_filePath = path;
    m_line = line;
    emitNodeSortKeyChanged();
    emitNodeUpdated();
}

NodeType Node::nodeType() const
{
    return m_nodeType;
}

int Node::priority() const
{
    return m_priority;
}

/*!
  The project that owns and manages the node. It is the first project in the list
  of ancestors.
  */
ProjectNode *Node::parentProjectNode() const
{
    if (!m_parentFolderNode)
        return nullptr;
    auto pn = m_parentFolderNode->asProjectNode();
    if (pn)
        return pn;
    return m_parentFolderNode->parentProjectNode();
}

/*!
  The parent in the node hierarchy.
  */
FolderNode *Node::parentFolderNode() const
{
    return m_parentFolderNode;
}

ProjectNode *Node::managingProject()
{
    if (asSessionNode())
        return nullptr;
    ProjectNode *pn = parentProjectNode();
    return pn ? pn : asProjectNode(); // projects manage themselves...
}

const ProjectNode *Node::managingProject() const
{
    return const_cast<Node *>(this)->managingProject();
}

/*!
  The path of the file or folder in the filesystem the node represents.
  */
const Utils::FileName &Node::filePath() const
{
    return m_filePath;
}

int Node::line() const
{
    return m_line;
}

QString Node::displayName() const
{
    return filePath().fileName();
}

QString Node::tooltip() const
{
    return filePath().toUserOutput();
}

bool Node::isEnabled() const
{
    if (!m_isEnabled)
        return false;
    FolderNode *parent = parentFolderNode();
    return parent ? parent->isEnabled() : true;
}

QList<ProjectAction> Node::supportedActions(Node *node) const
{
    QList<ProjectAction> list = parentFolderNode()->supportedActions(node);
    list.append(InheritedFromParent);
    return list;
}

void Node::setEnabled(bool enabled)
{
    if (m_isEnabled == enabled)
        return;
    m_isEnabled = enabled;
    emitNodeUpdated();
}

void Node::emitNodeUpdated()
{
    if (parentFolderNode())
        ProjectTree::instance()->emitNodeUpdated(this);
}

Node *Node::trim(const QSet<Node *> &keepers)
{
    return keepers.contains(this) ? nullptr : this;
}

bool Node::sortByPath(Node *a, Node *b)
{
    return a->filePath() < b->filePath();
}

void Node::setParentFolderNode(FolderNode *parentFolder)
{
    m_parentFolderNode = parentFolder;
}

/*!
  \class ProjectExplorer::FileNode

  \brief The FileNode class is an in-memory presentation of a file.

  All file nodes are leaf nodes.

  \sa ProjectExplorer::FolderNode, ProjectExplorer::ProjectNode
*/

FileNode::FileNode(const Utils::FileName &filePath,
                   const FileType fileType,
                   bool generated, int line) : Node(NodeType::File, filePath, line),
    m_fileType(fileType),
    m_generated(generated)
{
    if (fileType == FileType::Project)
        setPriority(DefaultProjectFilePriority);
    else
        setPriority(DefaultFilePriority);
}

FileType FileNode::fileType() const
{
    return m_fileType;
}

/*!
  Returns \c true if the file is automatically generated by a compile step.
  */
bool FileNode::isGenerated() const
{
    return m_generated;
}

static QList<FileNode *> scanForFilesRecursively(const Utils::FileName &directory,
                                                 const std::function<FileNode *(const Utils::FileName &)> factory,
                                                 QSet<QString> &visited, QFutureInterface<QList<FileNode*>> *future,
                                                 double progressStart, double progressRange)
{
    QList<FileNode *> result;

    const QDir baseDir = QDir(directory.toString());

    // Do not follow directory loops:
    const int visitedCount = visited.count();
    visited.insert(baseDir.canonicalPath());
    if (visitedCount == visited.count())
        return result;

    const Core::IVersionControl *vcsControl
            = Core::VcsManager::findVersionControlForDirectory(baseDir.absolutePath(), nullptr);
    const QList<QFileInfo> entries = baseDir.entryInfoList(QStringList(), QDir::AllEntries|QDir::NoDotAndDotDot);
    double progress = 0;
    const double progressIncrement = progressRange / static_cast<double>(entries.count());
    int lastIntProgress = 0;
    for (const QFileInfo &entry : entries) {
        if (future && future->isCanceled())
            return result;

        const Utils::FileName entryName = Utils::FileName::fromString(entry.absoluteFilePath());
        if (!vcsControl || !vcsControl->isVcsFileOrDirectory(entryName)) {
            if (entry.isDir())
                result.append(scanForFilesRecursively(entryName, factory, visited, future, progress, progressIncrement));
            else
                result.append(factory(entryName));
        }
        if (future) {
            progress += progressIncrement;
            const int intProgress = std::min(static_cast<int>(progressStart + progress), future->progressMaximum());
            if (lastIntProgress < intProgress) {
                future->setProgressValue(intProgress);
                lastIntProgress = intProgress;
            }
        }
    }
    if (future)
        future->setProgressValue(std::min(static_cast<int>(progressStart + progressRange), future->progressMaximum()));
    return result;
}

QList<FileNode *> FileNode::scanForFiles(const Utils::FileName &directory,
                                         const std::function<FileNode *(const Utils::FileName &)> factory,
                                         QFutureInterface<QList<FileNode*>> *future)
{
    QSet<QString> visited;
    future->setProgressRange(0, 1000000);
    return scanForFilesRecursively(directory, factory, visited, future, 0.0, 1000000.0);
}

/*!
  \class ProjectExplorer::FolderNode

  In-memory presentation of a folder. Note that the node itself + all children (files and folders) are "managed" by the owning project.

  \sa ProjectExplorer::FileNode, ProjectExplorer::ProjectNode
*/
FolderNode::FolderNode(const Utils::FileName &folderPath, NodeType nodeType, const QString &displayName) :
    Node(nodeType, folderPath, -1),
    m_displayName(displayName)
{
    setPriority(DefaultFolderPriority);
    if (m_displayName.isEmpty())
        m_displayName = folderPath.toUserOutput();
}

FolderNode::~FolderNode()
{
    qDeleteAll(m_folderNodes);
    qDeleteAll(m_fileNodes);
}

/*!
    Contains the display name that should be used in a view.
    \sa setFolderName()
 */

QString FolderNode::displayName() const
{
    return m_displayName;
}

/*!
  Contains the icon that should be used in a view. Default is the directory icon
 (QStyle::S_PDirIcon).
  s\a setIcon()
 */
QIcon FolderNode::icon() const
{
    // Instantiating the Icon provider is expensive.
    if (m_icon.isNull())
        m_icon = Core::FileIconProvider::icon(QFileIconProvider::Folder);
    return m_icon;
}

Node *FolderNode::trim(const QSet<Node *> &keepers)
{
    if (keepers.contains(this))
        return nullptr;

    bool keepThis = false;
    QList<Node *> toTrim = Utils::transform(m_fileNodes, [&keepers](Node *n) { return n->trim(keepers); });
    int count = toTrim.count();
    toTrim = Utils::filtered(toTrim, [](const Node *n) { return n; });
    keepThis = (count != toTrim.count());
    removeFileNodes(Utils::transform(toTrim, [](Node *n) { return static_cast<FileNode *>(n); }));

    toTrim = Utils::transform(m_folderNodes, [&keepers](Node *n) { return n->trim(keepers); });
    count = toTrim.count();
    toTrim = Utils::filtered(toTrim, [](const Node *n) { return n; });
    keepThis = keepThis || (count != toTrim.count());
    removeFolderNodes(Utils::transform(toTrim, [](Node *n) { return static_cast<FolderNode *>(n); }));
    return keepThis ? nullptr : this;
}

QList<FileNode*> FolderNode::fileNodes() const
{
    return m_fileNodes;
}

FileNode *FolderNode::fileNode(const Utils::FileName &file) const
{
    return Utils::findOrDefault(m_fileNodes, [&file](const FileNode *fn) {
        return fn->filePath() == file;
    });
}

FileNode *FolderNode::recursiveFileNode(const Utils::FileName &file) const
{
    Utils::FileName dir = file.parentDir();

    const QDir thisDir(filePath().toString());
    QString relativePath = thisDir.relativeFilePath(dir.toString());
    if (relativePath == ".")
        relativePath.clear();
    QStringList parts = relativePath.split('/', QString::SkipEmptyParts);
    const ProjectExplorer::FolderNode *parent = this;
    foreach (const QString &part, parts) {
        dir.appendPath(part);
        // Find folder in subFolders
        parent = Utils::findOrDefault(parent->folderNodes(), [&dir](const FolderNode *fn) {
            return fn->filePath() == dir;
        });
        if (!parent)
            return nullptr;
    }
    return parent->fileNode(file);
}

QList<FileNode *> FolderNode::recursiveFileNodes() const
{
    QList<FileNode *> result = fileNodes();
    foreach (ProjectExplorer::FolderNode *folder, folderNodes())
        result.append(folder->recursiveFileNodes());
    return result;
}

QList<FolderNode*> FolderNode::folderNodes() const
{
    return m_folderNodes;
}

FolderNode *FolderNode::folderNode(const Utils::FileName &directory) const
{
    return Utils::findOrDefault(m_folderNodes, [&directory](const FolderNode *fn) {
        return fn->filePath() == directory;
    });
}

FolderNode *FolderNode::recursiveFindOrCreateFolderNode(const QString &directory,
                                                        const Utils::FileName &overrideBaseDir)
{
    Utils::FileName path = overrideBaseDir.isEmpty() ? filePath() : overrideBaseDir;
    QString workPath;
    if (path.isEmpty() || path.toFileInfo().isRoot()) {
        workPath = directory;
    } else {
        QDir parentDir(path.toString());
        workPath = parentDir.relativeFilePath(directory);
        if (workPath == ".")
            workPath.clear();
    }
    const QStringList parts = workPath.split('/', QString::SkipEmptyParts);

    ProjectExplorer::FolderNode *parent = this;
    foreach (const QString &part, parts) {
        path.appendPath(part);
        // Find folder in subFolders
        FolderNode *next = parent->folderNode(path);
        if (!next) {
            // No FolderNode yet, so create it
            auto tmp = new ProjectExplorer::FolderNode(path);
            tmp->setDisplayName(part);
            parent->addFolderNodes(QList<ProjectExplorer::FolderNode *>({ tmp }));
            next = tmp;
        }
        parent = next;
    }
    return parent;
}

void FolderNode::buildTree(QList<FileNode *> &files, const Utils::FileName &overrideBaseDir)
{
    // Gather old list
    QList<ProjectExplorer::FileNode *> oldFiles = recursiveFileNodes();
    Utils::sort(oldFiles, Node::sortByPath);
    Utils::sort(files, Node::sortByPath);

    QList<ProjectExplorer::FileNode *> added;
    QList<ProjectExplorer::FileNode *> deleted;

    ProjectExplorer::compareSortedLists(oldFiles, files, deleted, added, Node::sortByPath);

    qDeleteAll(ProjectExplorer::subtractSortedList(files, added, Node::sortByPath));

    QHash<ProjectExplorer::FolderNode *, QList<ProjectExplorer::FileNode *> > addedFolderMapping;
    QHash<ProjectExplorer::FolderNode *, QList<ProjectExplorer::FileNode *> > deletedFolderMapping;

    // add added nodes
    foreach (ProjectExplorer::FileNode *fn, added) {
        // Get relative path to rootNode
        QString parentDir = fn->filePath().toFileInfo().absolutePath();
        ProjectExplorer::FolderNode *folder
                = recursiveFindOrCreateFolderNode(parentDir, overrideBaseDir);
        addedFolderMapping[folder] << fn;
    }

    for (auto i = addedFolderMapping.constBegin(); i != addedFolderMapping.constEnd(); ++i)
        i.key()->addFileNodes(i.value());

    // remove old file nodes and check whether folder nodes can be removed
    foreach (ProjectExplorer::FileNode *fn, deleted)
        deletedFolderMapping[fn->parentFolderNode()] << fn;

    for (auto i = deletedFolderMapping.constBegin(); i != deletedFolderMapping.constEnd(); ++i) {
        ProjectExplorer::FolderNode *parent = i.key();
        parent->removeFileNodes(i.value());

        if (parent == this) // Never delete this node!
            continue;

        // Check for empty parent
        while (parent->folderNodes().isEmpty() && parent->fileNodes().isEmpty()) {
            ProjectExplorer::FolderNode *grandparent = parent->parentFolderNode();
            grandparent->removeFolderNodes(QList<ProjectExplorer::FolderNode *>() << parent);
            parent = grandparent;
            if (parent == this)
                break;
        }
    }
}

void FolderNode::accept(NodesVisitor *visitor)
{
    visitor->visitFolderNode(this);
    foreach (FolderNode *subFolder, m_folderNodes)
        subFolder->accept(visitor);
}

void FolderNode::setDisplayName(const QString &name)
{
    if (m_displayName == name)
        return;
    emitNodeSortKeyAboutToChange();
    m_displayName = name;
    emitNodeSortKeyChanged();
    emitNodeUpdated();
}

void FolderNode::setIcon(const QIcon &icon)
{
    m_icon = icon;
}

QString FolderNode::addFileFilter() const
{
    return parentFolderNode()->addFileFilter();
}

bool FolderNode::addFiles(const QStringList &filePaths, QStringList *notAdded)
{
    ProjectNode *pn = managingProject();
    if (pn)
        return pn->addFiles(filePaths, notAdded);
    return false;
}

bool FolderNode::removeFiles(const QStringList &filePaths, QStringList *notRemoved)
{
    ProjectNode *pn = managingProject();
    if (pn)
        return pn->removeFiles(filePaths, notRemoved);
    return false;
}

bool FolderNode::deleteFiles(const QStringList &filePaths)
{
    ProjectNode *pn = managingProject();
    if (pn)
        return pn->deleteFiles(filePaths);
    return false;
}

bool FolderNode::canRenameFile(const QString &filePath, const QString &newFilePath)
{
    ProjectNode *pn = managingProject();
    if (pn)
        return pn->canRenameFile(filePath, newFilePath);
    return false;
}

bool FolderNode::renameFile(const QString &filePath, const QString &newFilePath)
{
    ProjectNode *pn = managingProject();
    if (pn)
        return pn->renameFile(filePath, newFilePath);
    return false;
}

FolderNode::AddNewInformation FolderNode::addNewInformation(const QStringList &files, Node *context) const
{
    Q_UNUSED(files);
    return AddNewInformation(displayName(), context == this ? 120 : 100);
}

/*!
  Adds file nodes specified by \a files to the internal list of the folder
  and emits the corresponding signals from the projectNode.

  This function should be called within an implementation of the public function
  addFiles.
*/

void FolderNode::addFileNodes(const QList<FileNode *> &files)
{
    Q_ASSERT(managingProject());

    if (files.isEmpty())
        return;

    ProjectTree::instance()->emitFilesAboutToBeAdded(this, files);

    foreach (FileNode *file, files) {
        QTC_ASSERT(!file->parentFolderNode(),
                   qDebug("File node has already a parent folder"));

        file->setParentFolderNode(this);
        // Now find the correct place to insert file
        if (m_fileNodes.count() == 0
                || m_fileNodes.last() < file) {
            // empty list or greater then last node
            m_fileNodes.append(file);
        } else {
            auto it = std::lower_bound(m_fileNodes.begin(), m_fileNodes.end(), file);
            m_fileNodes.insert(it, file);
        }
    }

    ProjectTree::instance()->emitFilesAdded(this);
}

/*!
  Removes \a files from the internal list and emits the corresponding signals.

  All objects in the \a files list are deleted.
  This function should be called within an implementation of the public function
  removeFiles.
*/

void FolderNode::removeFileNodes(const QList<FileNode *> &files)
{
    Q_ASSERT(managingProject());

    if (files.isEmpty())
        return;

    QList<FileNode*> toRemove = files;
    Utils::sort(toRemove);

    ProjectTree::instance()->emitFilesAboutToBeRemoved(this, toRemove);

    auto toRemoveIter = toRemove.constBegin();
    auto filesIter = m_fileNodes.begin();
    for (; toRemoveIter != toRemove.constEnd(); ++toRemoveIter) {
        while (*filesIter != *toRemoveIter) {
            ++filesIter;
            QTC_ASSERT(filesIter != m_fileNodes.end(),
                       qDebug("File to remove is not part of specified folder!"));
        }
        delete *filesIter;
        filesIter = m_fileNodes.erase(filesIter);
    }

    ProjectTree::instance()->emitFilesRemoved(this);
}

/*!
  Adds folder nodes specified by \a subFolders to the node hierarchy below
  \a parentFolder and emits the corresponding signals.
*/
void FolderNode::addFolderNodes(const QList<FolderNode*> &subFolders)
{
    Q_ASSERT(managingProject());

    if (subFolders.isEmpty())
        return;

    ProjectTree::instance()->emitFoldersAboutToBeAdded(this, subFolders);
    foreach (FolderNode *folder, subFolders) {
        QTC_ASSERT(!folder->parentFolderNode(),
                   qDebug("Project node has already a parent folder"));
        folder->setParentFolderNode(this);

        // Find the correct place to insert
        if (m_folderNodes.count() == 0
                || m_folderNodes.last() < folder) {
            // empty list or greater then last node
            m_folderNodes.append(folder);
        } else {
            // Binary Search for insertion point
            auto it = std::lower_bound(m_folderNodes.begin(), m_folderNodes.end(), folder);
            m_folderNodes.insert(it, folder);
        }

        // project nodes have to be added via addProjectNodes
        QTC_ASSERT(folder->nodeType() != NodeType::Project,
                   qDebug("project nodes have to be added via addProjectNodes"));
    }

    ProjectTree::instance()->emitFoldersAdded(this);
}

/*!
  Removes file nodes specified by \a subFolders from the node hierarchy and emits
  the corresponding signals.

  All objects in the \a subFolders list are deleted.
*/
void FolderNode::removeFolderNodes(const QList<FolderNode*> &subFolders)
{
    Q_ASSERT(managingProject());

    if (subFolders.isEmpty())
        return;

    QList<FolderNode*> toRemove = subFolders;
    Utils::sort(toRemove);

    ProjectTree::instance()->emitFoldersAboutToBeRemoved(this, toRemove);

    auto toRemoveIter = toRemove.constBegin();
    auto folderIter = m_folderNodes.begin();
    for (; toRemoveIter != toRemove.constEnd(); ++toRemoveIter) {
        QTC_ASSERT((*toRemoveIter)->nodeType() != NodeType::Project,
                   qDebug("project nodes have to be removed via removeProjectNodes"));
        while (*folderIter != *toRemoveIter) {
            ++folderIter;
            QTC_ASSERT(folderIter != m_folderNodes.end(),
                       qDebug("Folder to remove is not part of specified folder!"));
        }
        delete *folderIter;
        folderIter = m_folderNodes.erase(folderIter);
    }

    ProjectTree::instance()->emitFoldersRemoved(this);
}

bool FolderNode::showInSimpleTree() const
{
    return false;
}

/*!
  \class ProjectExplorer::VirtualFolderNode

  In-memory presentation of a virtual folder.
  Note that the node itself + all children (files and folders) are "managed" by the owning project.
  A virtual folder does not correspond to a actual folder on the file system. See for example the
  sources, headers and forms folder the QmakeProjectManager creates
  VirtualFolderNodes are always sorted before FolderNodes and are sorted according to their priority.

  \sa ProjectExplorer::FileNode, ProjectExplorer::ProjectNode
*/
VirtualFolderNode::VirtualFolderNode(const Utils::FileName &folderPath, int priority) :
    FolderNode(folderPath, NodeType::VirtualFolder, QString())
{
    setPriority(priority);
}

/*!
  \class ProjectExplorer::ProjectNode

  \brief The ProjectNode class is an in-memory presentation of a Project.

  A concrete subclass must implement the persistent data.

  \sa ProjectExplorer::FileNode, ProjectExplorer::FolderNode
*/

/*!
  Creates an uninitialized project node object.
  */
ProjectNode::ProjectNode(const Utils::FileName &projectFilePath) :
    FolderNode(projectFilePath, NodeType::Project)
{
    setPriority(DefaultProjectPriority);
    setDisplayName(projectFilePath.fileName());
}

QString ProjectNode::vcsTopic() const
{
    const QString dir = filePath().toFileInfo().absolutePath();

    if (Core::IVersionControl *const vc =
            Core::VcsManager::findVersionControlForDirectory(dir))
        return vc->vcsTopic(dir);

    return QString();
}

bool ProjectNode::canAddSubProject(const QString &proFilePath) const
{
    Q_UNUSED(proFilePath)
    return false;
}

bool ProjectNode::addSubProjects(const QStringList &proFilePaths)
{
    Q_UNUSED(proFilePaths)
    return false;
}

bool ProjectNode::removeSubProjects(const QStringList &proFilePaths)
{
    Q_UNUSED(proFilePaths)
    return false;
}

bool ProjectNode::addFiles(const QStringList &filePaths, QStringList *notAdded)
{
    Q_UNUSED(filePaths)
    Q_UNUSED(notAdded)
    return false;
}

bool ProjectNode::removeFiles(const QStringList &filePaths, QStringList *notRemoved)
{
    Q_UNUSED(filePaths)
    Q_UNUSED(notRemoved)
    return false;
}

bool ProjectNode::deleteFiles(const QStringList &filePaths)
{
    Q_UNUSED(filePaths)
    return false;
}

bool ProjectNode::canRenameFile(const QString &filePath, const QString &newFilePath)
{
    Q_UNUSED(filePath);
    Q_UNUSED(newFilePath);
    return true;
}

bool ProjectNode::renameFile(const QString &filePath, const QString &newFilePath)
{
    Q_UNUSED(filePath)
    Q_UNUSED(newFilePath)
    return false;
}

bool ProjectNode::deploysFolder(const QString &folder) const
{
    Q_UNUSED(folder);
    return false;
}

/*!
  \function bool ProjectNode::runConfigurations() const

  Returns a list of \c RunConfiguration suitable for this node.
  */
QList<RunConfiguration *> ProjectNode::runConfigurations() const
{
    return QList<RunConfiguration *>();
}

void ProjectNode::accept(NodesVisitor *visitor)
{
    visitor->visitProjectNode(this);

    foreach (FolderNode *folder, m_folderNodes)
        folder->accept(visitor);
}

ProjectNode *ProjectNode::projectNode(const Utils::FileName &file) const
{
    return Utils::findOrDefault(m_projectNodes, [&file](const ProjectNode *fn) {
        return fn->filePath() == file;
    });
}

QList<ProjectNode*> ProjectNode::projectNodes() const
{
    return m_projectNodes;
}

/*!
  Adds project nodes specified by \a subProjects to the node hierarchy and
  emits the corresponding signals.
  */
void ProjectNode::addProjectNodes(const QList<ProjectNode*> &subProjects)
{
    if (!subProjects.isEmpty()) {
        QList<FolderNode*> folderNodes;
        foreach (ProjectNode *projectNode, subProjects)
            folderNodes << projectNode;

        ProjectTree::instance()->emitFoldersAboutToBeAdded(this, folderNodes);

        foreach (ProjectNode *project, subProjects) {
            QTC_ASSERT(!project->parentFolderNode() || project->parentFolderNode() == this,
                       qDebug("Project node has already a parent"));
            project->setParentFolderNode(this);
            m_folderNodes.append(project);
            m_projectNodes.append(project);
        }
        Utils::sort(m_folderNodes);
        Utils::sort(m_projectNodes);

        ProjectTree::instance()->emitFoldersAdded(this);
    }
}

/*!
  Removes project nodes specified by \a subProjects from the node hierarchy
  and emits the corresponding signals.

  All objects in the \a subProjects list are deleted.
*/

void ProjectNode::removeProjectNodes(const QList<ProjectNode*> &subProjects)
{
    if (!subProjects.isEmpty()) {
        QList<FolderNode*> toRemove;
        foreach (ProjectNode *projectNode, subProjects)
            toRemove << projectNode;
        Utils::sort(toRemove);

        ProjectTree::instance()->emitFoldersAboutToBeRemoved(this, toRemove);

        auto toRemoveIter = toRemove.constBegin();
        auto folderIter = m_folderNodes.begin();
        auto projectIter = m_projectNodes.begin();
        for (; toRemoveIter != toRemove.constEnd(); ++toRemoveIter) {
            while (*projectIter != *toRemoveIter) {
                ++projectIter;
                QTC_ASSERT(projectIter != m_projectNodes.end(),
                    qDebug("Project to remove is not part of specified folder!"));
            }
            while (*folderIter != *toRemoveIter) {
                ++folderIter;
                QTC_ASSERT(folderIter != m_folderNodes.end(),
                    qDebug("Project to remove is not part of specified folder!"));
            }
            delete *projectIter;
            projectIter = m_projectNodes.erase(projectIter);
            folderIter = m_folderNodes.erase(folderIter);
        }

        ProjectTree::instance()->emitFoldersRemoved(this);
    }
}

Node *ProjectNode::trim(const QSet<Node *> &keepers)
{
    if (keepers.contains(this))
        return nullptr;

    QList<Node *> toTrim
            = Utils::transform(m_projectNodes, [&keepers](Node *n) { return n->trim(keepers); });
    int count = toTrim.count();
    toTrim = Utils::filtered(toTrim, [](Node *n) { return n; });
    removeProjectNodes(Utils::transform(toTrim, [](Node *n) { return static_cast<ProjectNode *>(n); }));

    if (!FolderNode::trim(keepers))
        return nullptr;

    return (toTrim.count() != count) ? nullptr : this;
}

/*!
  \class ProjectExplorer::SessionNode
*/

SessionNode::SessionNode() :
    FolderNode(Utils::FileName::fromString("session"), NodeType::Session)
{ }

QList<ProjectAction> SessionNode::supportedActions(Node *node) const
{
    Q_UNUSED(node)
    return QList<ProjectAction>();
}


void SessionNode::accept(NodesVisitor *visitor)
{
    visitor->visitSessionNode(this);
    foreach (ProjectNode *project, m_projectNodes)
        project->accept(visitor);
}

bool SessionNode::showInSimpleTree() const
{
    return true;
}

void SessionNode::projectDisplayNameChanged(Node *node)
{
    ProjectTree::instance()->emitNodeSortKeyAboutToChange(node);
    ProjectTree::instance()->emitNodeSortKeyChanged(node);
}

QList<ProjectNode*> SessionNode::projectNodes() const
{
    return m_projectNodes;
}

QString SessionNode::addFileFilter() const
{
    return QString::fromLatin1("*.c; *.cc; *.cpp; *.cp; *.cxx; *.c++; *.h; *.hh; *.hpp; *.hxx;");
}

void SessionNode::addProjectNodes(const QList<ProjectNode*> &projectNodes)
{
    if (!projectNodes.isEmpty()) {
        QList<FolderNode*> folderNodes;
        foreach (ProjectNode *projectNode, projectNodes)
            folderNodes << projectNode;

        ProjectTree::instance()->emitFoldersAboutToBeAdded(this, folderNodes);

        foreach (ProjectNode *project, projectNodes) {
            QTC_ASSERT(!project->parentFolderNode(),
                qDebug("Project node has already a parent folder"));
            project->setParentFolderNode(this);
            m_folderNodes.append(project);
            m_projectNodes.append(project);
        }

        Utils::sort(m_folderNodes);
        Utils::sort(m_projectNodes);

        ProjectTree::instance()->emitFoldersAdded(this);
   }
}

void SessionNode::removeProjectNodes(const QList<ProjectNode*> &projectNodes)
{
    if (!projectNodes.isEmpty()) {
        QList<FolderNode*> toRemove;
        foreach (ProjectNode *projectNode, projectNodes)
            toRemove << projectNode;

        Utils::sort(toRemove);

        ProjectTree::instance()->emitFoldersAboutToBeRemoved(this, toRemove);

        auto toRemoveIter = toRemove.constBegin();
        auto folderIter = m_folderNodes.begin();
        auto projectIter = m_projectNodes.begin();
        for (; toRemoveIter != toRemove.constEnd(); ++toRemoveIter) {
            while (*projectIter != *toRemoveIter) {
                ++projectIter;
                QTC_ASSERT(projectIter != m_projectNodes.end(),
                    qDebug("Project to remove is not part of specified folder!"));
            }
            while (*folderIter != *toRemoveIter) {
                ++folderIter;
                QTC_ASSERT(folderIter != m_folderNodes.end(),
                    qDebug("Project to remove is not part of specified folder!"));
            }
            projectIter = m_projectNodes.erase(projectIter);
            folderIter = m_folderNodes.erase(folderIter);
        }

        ProjectTree::instance()->emitFoldersRemoved(this);
    }
}

} // namespace ProjectExplorer
