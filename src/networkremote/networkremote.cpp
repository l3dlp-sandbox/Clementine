/* This file is part of Clementine.
   Copyright 2012, Andreas Muttscheller <asfa194@gmail.com>

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "core/logging.h"
#include "covers/currentartloader.h"
#include "playlist/playlistmanager.h"

#include "networkremote.h"

#include <QDataStream>
#include <QSettings>

const char* NetworkRemote::kSettingsGroup = "NetworkRemote";
const int NetworkRemote::kDefaultServerPort = 5500;

NetworkRemote::NetworkRemote(Application* app)
  : app_(app)
{
  signals_connected_ = false;
}


NetworkRemote::~NetworkRemote() {
  StopServer();
  delete incoming_data_parser_;
  delete outgoing_data_creator_;
}

void NetworkRemote::ReadSettings() {
  QSettings s;

  s.beginGroup(NetworkRemote::kSettingsGroup);
  use_remote_ = s.value("use_remote").toBool();
  port_       = s.value("port").toInt();
  if (port_ == 0) {
    port_ = kDefaultServerPort;
  }
  s.endGroup();
}

void NetworkRemote::SetupServer() {
  server_ = new QTcpServer();
  incoming_data_parser_  = new IncomingDataParser(app_);
  outgoing_data_creator_ = new OutgoingDataCreator(app_);

  outgoing_data_creator_->SetClients(&clients_);

  connect(app_->current_art_loader(),
          SIGNAL(ArtLoaded(const Song&, const QString&, const QImage&)),
          outgoing_data_creator_,
          SLOT(CurrentSongChanged(const Song&, const QString&, const QImage&)));
}

void NetworkRemote::StartServer() {
  if (!app_) {
    qLog(Error) << "Start Server called without having an application!";
    return;
  }
  // Check if user desires to start a network remote server
  ReadSettings();
  if (!use_remote_) {
    qLog(Info) << "Network Remote deactivated";
    return;
  }

  qLog(Info) << "Starting network remote";

  connect(server_, SIGNAL(newConnection()), this, SLOT(AcceptConnection()));

  server_->listen(QHostAddress::Any, port_);

  qLog(Info) << "Listening on port " << port_;
}

void NetworkRemote::StopServer() {
  if (server_->isListening()) {
    server_->close();
    clients_.clear();
    expected_length_.clear();
    reading_protobuf_.clear();

    // Delete all QBuffers
    foreach (QBuffer* b, buffer_) {
      delete b;
    }
    buffer_.clear();
  }
}

void NetworkRemote::ReloadSettings() {
  StopServer();
  StartServer();
}

void NetworkRemote::AcceptConnection() {
  if (!signals_connected_) {
    signals_connected_ = true;

    // Setting up the signals, but only once
    connect(incoming_data_parser_, SIGNAL(SendClementineInfos()),
            outgoing_data_creator_, SLOT(SendClementineInfos()));
    connect(incoming_data_parser_, SIGNAL(SendFirstData()),
            outgoing_data_creator_, SLOT(SendFirstData()));
    connect(incoming_data_parser_, SIGNAL(SendAllPlaylists()),
            outgoing_data_creator_, SLOT(SendAllPlaylists()));
    connect(incoming_data_parser_, SIGNAL(SendPlaylistSongs(int)),
            outgoing_data_creator_, SLOT(SendPlaylistSongs(int)));

    connect(app_->playlist_manager(), SIGNAL(ActiveChanged(Playlist*)),
            outgoing_data_creator_, SLOT(ActiveChanged(Playlist*)));
    connect(app_->playlist_manager(), SIGNAL(PlaylistChanged(Playlist*)),
            outgoing_data_creator_, SLOT(PlaylistChanged(Playlist*)));

    connect(app_->player(), SIGNAL(VolumeChanged(int)), outgoing_data_creator_,
            SLOT(VolumeChanged(int)));
    connect(app_->player()->engine(), SIGNAL(StateChanged(Engine::State)),
            outgoing_data_creator_, SLOT(StateChanged(Engine::State)));
  }
  QTcpSocket* client = server_->nextPendingConnection();

  clients_.push_back(client);
  reading_protobuf_.append(false);
  expected_length_.append(0);
  QBuffer* buf = new QBuffer();
  buf->open(QIODevice::ReadWrite);
  buffer_.push_back(buf);

  // Connect to the slot IncomingData when receiving data
  connect(client, SIGNAL(readyRead()), this, SLOT(IncomingData()));
}

void NetworkRemote::IncomingData() {
  QTcpSocket* client =  static_cast<QTcpSocket*>(QObject::sender());

  int which = clients_.indexOf(client);
  qDebug() << "Which:" << which;

  while (client->bytesAvailable()) {
    if (!reading_protobuf_[which]) {
      // Read the length of the next message
      QDataStream s(client);
      s >> expected_length_[which];

      reading_protobuf_[which] = true;
    }

    // Read some of the message
    buffer_[which]->write(
      client->read(expected_length_[which] - buffer_[which]->size()));

    // Did we get everything?
    if (buffer_[which]->size() == expected_length_[which]) {
      // Parse the message
      incoming_data_parser_->Parse(buffer_[which]->data());

      // Clear the buffer
      buffer_[which]->close();
      buffer_[which]->setData(QByteArray());
      buffer_[which]->open(QIODevice::ReadWrite);
      reading_protobuf_[which] = false;
    }
  }
}

void NetworkRemote::RemoveClient(int index) {
  // Remove client
  QTcpSocket* client = clients_[index];
  delete client;
  clients_.removeAt(index);

  // Remove QBuffer
  QBuffer* buf = buffer_[index];
  delete buf;
  buffer_.removeAt(index);

  // Remove other QList entries
  reading_protobuf_.removeAt(index);
  expected_length_.removeAt(index);
}