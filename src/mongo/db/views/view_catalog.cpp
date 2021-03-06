/**
*    Copyright (C) 2016 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/views/view_catalog.h"

#include <string>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/views/view.h"
#include "mongo/util/log.h"

namespace {
bool enableViews = true;
}  // namespace

namespace mongo {
ExportedServerParameter<bool, ServerParameterType::kStartupOnly> enableViewsParameter(
    ServerParameterSet::getGlobal(), "enableViews", &enableViews);

Status ViewCatalog::reloadIfNeeded(OperationContext* txn) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _reloadIfNeeded_inlock(txn);
}

Status ViewCatalog::_reloadIfNeeded_inlock(OperationContext* txn) {
    if (_valid.load())
        return Status::OK();

    LOG(1) << "reloading view catalog for database " << _durable->getName();

    // Need to reload, first clear our cache.
    _viewMap.clear();

    Status status = _durable->iterate(txn, [&](const BSONObj& view) {
        NamespaceString viewName(view["_id"].str());
        ViewDefinition def(
            viewName.db(), viewName.coll(), view["viewOn"].str(), view["pipeline"].Obj());
        _viewMap[viewName.ns()] = std::make_shared<ViewDefinition>(def);
    });
    _valid.store(status.isOK());
    return status;
}

Status ViewCatalog::_createOrUpdateView_inlock(OperationContext* txn,
                                               const NamespaceString& viewName,
                                               const NamespaceString& viewOn,
                                               const BSONArray& pipeline) {
    invariant(_valid.load());
    BSONObj viewDef =
        BSON("_id" << viewName.ns() << "viewOn" << viewOn.coll() << "pipeline" << pipeline);

    BSONObj ownedPipeline = pipeline.getOwned();
    auto view = std::make_shared<ViewDefinition>(
        viewName.db(), viewName.coll(), viewOn.coll(), ownedPipeline);

    // Check that the resulting dependency graph is acyclic and within the maximum depth.
    Status graphStatus = _upsertIntoGraph(txn, *(view.get()));
    if (!graphStatus.isOK()) {
        return graphStatus;
    }

    _durable->upsert(txn, viewName, viewDef);
    _viewMap[viewName.ns()] = view;
    txn->recoveryUnit()->onRollback([this, viewName]() {
        this->_viewMap.erase(viewName.ns());
        this->_viewGraphNeedsRefresh = true;
    });

    // We may get invalidated, but we're exclusively locked, so the change must be ours.
    txn->recoveryUnit()->onCommit([this]() { this->_valid.store(true); });
    return Status::OK();
}

Status ViewCatalog::_upsertIntoGraph(OperationContext* txn, const ViewDefinition& viewDef) {

    // Performs the insert into the graph.
    auto doInsert = [this, &txn](const ViewDefinition& viewDef, bool needsValidation) -> Status {
        // Parse the pipeline for this view to get the namespaces it references.
        AggregationRequest request(viewDef.viewOn(), viewDef.pipeline());
        boost::intrusive_ptr<ExpressionContext> expCtx = new ExpressionContext(txn, request);
        auto pipelineStatus = Pipeline::parse(viewDef.pipeline(), expCtx);
        if (!pipelineStatus.isOK()) {
            uassert(40255,
                    str::stream() << "Invalid pipeline for existing view " << viewDef.name().ns()
                                  << "; "
                                  << pipelineStatus.getStatus().reason(),
                    !needsValidation);
            return pipelineStatus.getStatus();
        }

        std::vector<NamespaceString> refs = pipelineStatus.getValue()->getInvolvedCollections();
        refs.push_back(viewDef.viewOn());

        if (needsValidation) {
            return _viewGraph.insertAndValidate(viewDef.name(), refs);
        } else {
            _viewGraph.insertWithoutValidating(viewDef.name(), refs);
            return Status::OK();
        }
    };

    if (_viewGraphNeedsRefresh) {
        _viewGraph.clear();
        for (auto&& iter : _viewMap) {
            auto status = doInsert(*(iter.second.get()), false);
            // If we cannot fully refresh the graph, we will keep '_viewGraphNeedsRefresh' true.
            if (!status.isOK()) {
                return status;
            }
        }
        // Only if the inserts completed without error will we no longer need a refresh.
        _viewGraphNeedsRefresh = false;
    }

    // Remove the view definition first in case this is an update. If it is not in the graph, it
    // is simply a no-op.
    _viewGraph.remove(viewDef.name());

    return doInsert(viewDef, true);
}

Status ViewCatalog::createView(OperationContext* txn,
                               const NamespaceString& viewName,
                               const NamespaceString& viewOn,
                               const BSONArray& pipeline) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (!enableViews)
        return Status(ErrorCodes::CommandNotSupported, "View support not enabled");

    if (viewName.db() != viewOn.db())
        return Status(ErrorCodes::BadValue,
                      "View must be created on a view or collection in the same database");

    if (_lookup_inlock(txn, StringData(viewName.ns())))
        return Status(ErrorCodes::NamespaceExists, "Namespace already exists");

    if (!NamespaceString::validCollectionName(viewOn.coll()))
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid name for 'viewOn': " << viewOn.coll());

    return _createOrUpdateView_inlock(txn, viewName, viewOn, pipeline);
}

Status ViewCatalog::modifyView(OperationContext* txn,
                               const NamespaceString& viewName,
                               const NamespaceString& viewOn,
                               const BSONArray& pipeline) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (viewName.db() != viewOn.db())
        return Status(ErrorCodes::BadValue,
                      "View must be created on a view or collection in the same database");

    ViewDefinition* viewPtr = _lookup_inlock(txn, viewName.ns());
    if (!viewPtr)
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "cannot modify missing view " << viewName.ns());

    if (!NamespaceString::validCollectionName(viewOn.coll()))
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid name for 'viewOn': " << viewOn.coll());

    ViewDefinition savedDefinition = *viewPtr;
    txn->recoveryUnit()->onRollback([this, txn, viewName, savedDefinition]() {
        this->_viewMap[viewName.ns()] = std::make_shared<ViewDefinition>(savedDefinition);
    });
    return _createOrUpdateView_inlock(txn, viewName, viewOn, pipeline);
}

Status ViewCatalog::dropView(OperationContext* txn, const NamespaceString& viewName) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Save a copy of the view definition in case we need to roll back.
    ViewDefinition* viewPtr = _lookup_inlock(txn, viewName.ns());
    if (!viewPtr) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "cannot drop missing view: " << viewName.ns()};
    }

    ViewDefinition savedDefinition = *viewPtr;

    invariant(_valid.load());
    _durable->remove(txn, viewName);
    _viewGraph.remove(savedDefinition.name());
    _viewMap.erase(viewName.ns());
    txn->recoveryUnit()->onRollback([this, txn, viewName, savedDefinition]() {
        this->_viewGraphNeedsRefresh = true;
        this->_viewMap[viewName.ns()] = std::make_shared<ViewDefinition>(savedDefinition);
    });

    // We may get invalidated, but we're exclusively locked, so the change must be ours.
    txn->recoveryUnit()->onCommit([this]() { this->_valid.store(true); });
    return Status::OK();
}

ViewDefinition* ViewCatalog::_lookup_inlock(OperationContext* txn, StringData ns) {
    uassertStatusOK(_reloadIfNeeded_inlock(txn));
    ViewMap::const_iterator it = _viewMap.find(ns);
    if (it != _viewMap.end()) {
        return it->second.get();
    }
    return nullptr;
}

ViewDefinition* ViewCatalog::lookup(OperationContext* txn, StringData ns) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _lookup_inlock(txn, ns);
}

StatusWith<ResolvedView> ViewCatalog::resolveView(OperationContext* txn,
                                                  const NamespaceString& nss) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    const NamespaceString* resolvedNss = &nss;
    std::vector<BSONObj> resolvedPipeline;

    for (int i = 0; i < ViewGraph::kMaxViewDepth; i++) {
        ViewDefinition* view = _lookup_inlock(txn, resolvedNss->ns());
        if (!view)
            return StatusWith<ResolvedView>({*resolvedNss, resolvedPipeline});

        resolvedNss = &(view->viewOn());

        // Prepend the underlying view's pipeline to the current working pipeline.
        const std::vector<BSONObj>& toPrepend = view->pipeline();
        resolvedPipeline.insert(resolvedPipeline.begin(), toPrepend.begin(), toPrepend.end());

        // If the first stage is a $collStats, then we return early with the viewOn namespace.
        if (toPrepend.size() > 0 && !toPrepend[0]["$collStats"].eoo()) {
            return StatusWith<ResolvedView>({*resolvedNss, resolvedPipeline});
        }
    }

    return {ErrorCodes::ViewDepthLimitExceeded,
            str::stream() << "View depth too deep or view cycle detected; maximum depth is "
                          << ViewGraph::kMaxViewDepth};
}
}  // namespace mongo
