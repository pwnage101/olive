/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2019 Olive Team

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

***/

#include "renderbackend.h"

#include <QDateTime>
#include <QThread>

#include "config/config.h"
#include "core.h"
#include "task/conform/conform.h"
#include "task/taskmanager.h"
#include "window/mainwindow/mainwindow.h"

OLIVE_NAMESPACE_ENTER

RenderBackend::RenderBackend(QObject *parent) :
  QObject(parent),
  viewer_node_(nullptr),
  update_with_graph_(false),
  preview_job_time_(0),
  render_mode_(RenderMode::kOnline)
{
}

RenderBackend::~RenderBackend()
{
  Close();
}

void RenderBackend::SetViewerNode(ViewerOutput *viewer_node)
{
  if (viewer_node_ == viewer_node) {
    return;
  }

  ViewerOutput* old_viewer = viewer_node_;
  if (!viewer_node) {
    // If setting to null, set it here before we wait for jobs to finish
    viewer_node_ = viewer_node;
  }

  if (old_viewer) {
    // Delete all of our copied nodes
    pool_.clear();
    pool_.waitForDone();

    // Cancel all tickets
    foreach (RenderTicketPtr t, render_queue_) {
      t->Cancel();
    }
    render_queue_.clear();

    // Delete all the nodes
    foreach (Node* c, copy_map_) {
      c->deleteLater();
    }
    copy_map_.clear();
    copied_viewer_node_ = nullptr;
    graph_update_queue_.clear();

    disconnect(old_viewer,
               &ViewerOutput::GraphChangedFrom,
               this,
               &RenderBackend::NodeGraphChanged);
  }

  if (viewer_node) {
    // If setting to non-null, set it now
    viewer_node_ = viewer_node;
  }

  if (viewer_node_) {
    // Copy graph
    copied_viewer_node_ = static_cast<ViewerOutput*>(viewer_node_->copy());
    copy_map_.insert(viewer_node_, copied_viewer_node_);

    NodeGraphChanged(viewer_node_->texture_input());
    NodeGraphChanged(viewer_node_->samples_input());
    ProcessUpdateQueue();

    if (update_with_graph_) {
      connect(viewer_node_,
              &ViewerOutput::GraphChangedFrom,
              this,
              &RenderBackend::NodeGraphChanged);
    }
  }
}

void RenderBackend::ClearVideoQueue()
{
  foreach (RenderTicketPtr t, render_queue_) {
    t->Cancel();
  }
  render_queue_.clear();
}

RenderTicketPtr RenderBackend::Hash(const QVector<rational> &times)
{
  if (!viewer_node_) {
    return nullptr;
  }

  RenderTicketPtr ticket = std::make_shared<RenderTicket>(RenderTicket::kTypeHash,
                                                          QVariant::fromValue(times));

  render_queue_.push_back(ticket);

  QMetaObject::invokeMethod(this, "RunNextJob", Qt::QueuedConnection);

  return ticket;
}

RenderTicketPtr RenderBackend::RenderFrame(const rational &time)
{
  if (!viewer_node_) {
    return nullptr;
  }

  RenderTicketPtr ticket = std::make_shared<RenderTicket>(RenderTicket::kTypeVideo,
                                                          QVariant::fromValue(time));

  render_queue_.push_back(ticket);

  QMetaObject::invokeMethod(this, "RunNextJob", Qt::QueuedConnection);

  return ticket;
}

RenderTicketPtr RenderBackend::RenderAudio(const TimeRange &r)
{
  if (!viewer_node_) {
    return nullptr;
  }

  RenderTicketPtr ticket = std::make_shared<RenderTicket>(RenderTicket::kTypeAudio,
                                                          QVariant::fromValue(r));

  render_queue_.push_back(ticket);

  QMetaObject::invokeMethod(this, "RunNextJob", Qt::QueuedConnection);

  return ticket;
}

void RenderBackend::SetVideoParams(const VideoParams &params)
{
  video_params_ = params;
}

void RenderBackend::SetAudioParams(const AudioParams &params)
{
  audio_params_ = params;
}

void RenderBackend::SetVideoDownloadMatrix(const QMatrix4x4 &mat)
{
  video_download_matrix_ = mat;
}

std::list<TimeRange> RenderBackend::SplitRangeIntoChunks(const TimeRange &r)
{
  // FIXME: Magic number
  const int chunk_size = 2;

  std::list<TimeRange> split_ranges;

  int start_time = qFloor(r.in().toDouble() / static_cast<double>(chunk_size)) * chunk_size;
  int end_time = qCeil(r.out().toDouble() / static_cast<double>(chunk_size)) * chunk_size;

  for (int i=start_time; i<end_time; i+=chunk_size) {
    split_ranges.push_back(TimeRange(qMax(r.in(), rational(i)),
                                     qMin(r.out(), rational(i + chunk_size))));
  }

  return split_ranges;
}

void RenderBackend::NodeGraphChanged(NodeInput *source)
{
  // We need to determine:
  // - If we don't have this input, assume that it's coming soon and ignore it
  // - If we do, is this input a child of another input we're already copying?
  // - Or are any of the queued inputs children of this one?

  // First we need to find our copy of the input being queued
  Node* our_copy_node = copy_map_.value(source->parentNode());

  // If we don't have this node yet, assume it's coming in a later copy in which case it'll be
  // copied then
  if (!our_copy_node) {
    // Assert that there are updates coming
    Q_ASSERT(!graph_update_queue_.isEmpty());
    return;
  }

  // If we're here, we must have this node. Determine if we're already copying a "parent" of this
  for (int i=0; i<graph_update_queue_.size(); i++) {
    NodeInput* queued_input = graph_update_queue_.at(i);

    // If this input is already queued, nothing to be done
    if (source == queued_input) {
      return;
    }

    // Check if this dependency graph is already queued
    if (source->parentNode()->OutputsTo(queued_input, true, true)) {
      // In which case, no further copy is necessary
      return;
    }

    // Check if the source is a member of this array, in which case it'll be copied eventually anyway
    if (queued_input->IsArray()
        && static_cast<NodeInputArray*>(queued_input)->sub_params().contains(source)) {
      return;
    }

    // Check if this input supersedes an already queued input
    if (queued_input->parentNode()->OutputsTo(source, true, true)
        || (source->IsArray() && static_cast<NodeInputArray*>(source)->sub_params().contains(queued_input))) {
      // In which case, we don't need to queue it and can queue our own
      graph_update_queue_.removeAt(i);
      i--;
    }
  }

  graph_update_queue_.append(source);
}

void RenderBackend::Close()
{
  SetViewerNode(nullptr);

  for (int i=0;i<workers_.size();i++) {
    workers_.at(i).worker->deleteLater();
  }
  workers_.clear();
}

void RenderBackend::RunNextJob()
{
  // If queue is empty, nothing to be done
  if (render_queue_.empty()) {
    return;
  }

  // Check if params are valid
  if (!video_params_.is_valid()
      || !audio_params_.is_valid()) {
    qDebug() << "Failed to run job, parameters are invalid";
    return;
  }

  // If we have a value update queued, check if all workers are available and proceed from there
  if (update_with_graph_ && !graph_update_queue_.isEmpty()) {
    bool all_workers_available = true;

    foreach (const WorkerData& data, workers_) {
      if (data.busy) {
        all_workers_available = false;
        break;
      }
    }

    if (all_workers_available) {
      // Process queue
      ProcessUpdateQueue();
    } else {
      return;
    }
  }

  // If we have no workers allocated, allocate them now
  if (workers_.isEmpty()) {
    // Allocate workers here
    workers_.resize(pool_.maxThreadCount());

    for (int i=0;i<workers_.size();i++) {
      RenderWorker* worker = CreateNewWorker();

      connect(worker, &RenderWorker::FinishedJob, this, &RenderBackend::WorkerFinished);

      workers_.replace(i, {worker, false});
    }
  }

  // Start popping jobs off the queue
  for (int i=0;i<workers_.size();i++) {
    if (!workers_.at(i).busy) {
      // This worker is available, send it the job

      RenderWorker* worker = workers_[i].worker;

      workers_[i].busy = true;

      worker->SetVideoParams(video_params_);
      worker->SetAudioParams(audio_params_);
      worker->SetVideoDownloadMatrix(video_download_matrix_);
      worker->SetRenderMode(render_mode_);
      if (preview_job_time_) {
        worker->EnablePreviewGeneration(viewer_node_->audio_playback_cache(), preview_job_time_);
      }
      worker->SetCopyMap(&copy_map_);

      RenderTicketPtr ticket = render_queue_.front();
      render_queue_.pop_front();

      switch (ticket->GetType()) {
      case RenderTicket::kTypeHash:
        QtConcurrent::run(&pool_,
                          worker,
                          &RenderWorker::Hash,
                          ticket,
                          copied_viewer_node_,
                          ticket->GetTime().value<QVector<rational> >());
        break;
      case RenderTicket::kTypeVideo:
        QtConcurrent::run(&pool_,
                          worker,
                          &RenderWorker::RenderFrame,
                          ticket,
                          copied_viewer_node_,
                          ticket->GetTime().value<rational>());
        break;
      case RenderTicket::kTypeAudio:
        QtConcurrent::run(&pool_,
                          worker,
                          &RenderWorker::RenderAudio,
                          ticket,
                          copied_viewer_node_,
                          ticket->GetTime().value<TimeRange>());
        break;
      }

      if (render_queue_.empty()) {
        // No more jobs, can exit here
        break;
      }
    }
  }
}

//#define PRINT_UPDATE_QUEUE_INFO
void RenderBackend::ProcessUpdateQueue()
{
#ifdef PRINT_UPDATE_QUEUE_INFO
  qint64 t = QDateTime::currentMSecsSinceEpoch();
  qDebug() << "Processing update queue of" << graph_update_queue_.size() << "elements:";
#endif

  while (!graph_update_queue_.isEmpty()) {
    NodeInput* i = graph_update_queue_.takeFirst();
#ifdef PRINT_UPDATE_QUEUE_INFO
    qDebug() << " " << i->parentNode()->id() << i->id();
#endif
    CopyNodeInputValue(i);
  }

#ifdef PRINT_UPDATE_QUEUE_INFO
  qDebug() << "Update queue took:" << (QDateTime::currentMSecsSinceEpoch() - t);
#endif
}

void RenderBackend::WorkerFinished()
{
  RenderWorker* worker = static_cast<RenderWorker*>(sender());

  // Set busy state to false
  for (int i=0;i<workers_.size();i++) {
    if (workers_.at(i).worker == worker) {
      workers_[i].busy = false;
      break;
    }
  }

  if (viewer_node_) {
    RunNextJob();
  }
}

void RenderBackend::CopyNodeInputValue(NodeInput *input)
{
  // Find our copy of this parameter
  Node* our_copy_node = copy_map_.value(input->parentNode());
  Q_ASSERT(our_copy_node);
  NodeInput* our_copy = our_copy_node->GetInputWithID(input->id());

  // Copy the standard/keyframe values between these two inputs
  NodeInput::CopyValues(input,
                        our_copy,
                        false,
                        false);

  // Handle connections
  if (input->is_connected() || our_copy->is_connected()) {
    // If one of the inputs is connected, it's likely this change came from connecting or
    // disconnecting whatever was connected to it

    // We start by removing all old dependencies from the map
    QList<Node*> old_deps = our_copy->GetExclusiveDependencies();
    foreach (Node* i, old_deps) {
      copy_map_.take(copy_map_.key(i))->deleteLater();
    }

    // And clear any other edges
    while (!our_copy->edges().isEmpty()) {
      NodeParam::DisconnectEdge(our_copy->edges().first());
    }

    // Then we copy all node dependencies and connections (if there are any)
    CopyNodeMakeConnection(input, our_copy);
  }

  // Call on sub-elements too
  if (input->IsArray()) {
    foreach (NodeInput* i, static_cast<NodeInputArray*>(input)->sub_params()) {
      CopyNodeInputValue(i);
    }
  }
}

Node* RenderBackend::CopyNodeConnections(Node* src_node)
{
  // Check if this node is already in the map
  Node* dst_node = copy_map_.value(src_node);

  // If not, create it now
  if (!dst_node) {
    dst_node = src_node->copy();

    if (dst_node->IsTrack()) {
      // Hack that ensures the track type is set since we don't bother copying the whole timeline
      static_cast<TrackOutput*>(dst_node)->set_track_type(static_cast<TrackOutput*>(src_node)->track_type());
    }

    copy_map_.insert(src_node, dst_node);
  }

  // Make sure its values are copied
  Node::CopyInputs(src_node, dst_node, false);

  // Copy all connections
  QList<NodeInput*> src_node_inputs = src_node->GetInputsIncludingArrays();
  QList<NodeInput*> dst_node_inputs = dst_node->GetInputsIncludingArrays();

  for (int i=0;i<src_node_inputs.size();i++) {
    NodeInput* src_input = src_node_inputs.at(i);

    CopyNodeMakeConnection(src_input, dst_node_inputs.at(i));
  }

  return dst_node;
}

void RenderBackend::CopyNodeMakeConnection(NodeInput* src_input, NodeInput* dst_input)
{
  if (src_input->is_connected()) {
    Node* dst_node = CopyNodeConnections(src_input->get_connected_node());

    NodeOutput* corresponding_output = dst_node->GetOutputWithID(src_input->get_connected_output()->id());

    NodeParam::ConnectEdge(corresponding_output,
                           dst_input);
  }
}

OLIVE_NAMESPACE_EXIT
