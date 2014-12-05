/*
 * (C) Copyright 2014 Kurento (http://kurento.org/)
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE MediaElement
#include <boost/test/unit_test.hpp>
#include <MediaPipelineImpl.hpp>
#include <MediaElementImpl.hpp>
#include <ElementConnectionData.hpp>
#include <MediaType.hpp>

using namespace kurento;

BOOST_AUTO_TEST_CASE (connection_test)
{
  gst_init (NULL, NULL);
  std::shared_ptr <MediaPipelineImpl> pipe (new MediaPipelineImpl (
        boost::property_tree::ptree() ) );
  std::shared_ptr <MediaElementImpl> sink (new  MediaElementImpl (
        boost::property_tree::ptree(), pipe, "dummysink") );
  std::shared_ptr <MediaElementImpl> src (new  MediaElementImpl (
      boost::property_tree::ptree(), pipe, "dummysrc") );
  std::shared_ptr <MediaType> VIDEO (new MediaType (MediaType::VIDEO) );
  std::shared_ptr <MediaType> AUDIO (new MediaType (MediaType::AUDIO) );

  src->setName ("SOURCE");
  sink->setName ("SINK");

  src->connect (sink);
  auto connections = sink->getSourceConnections ();
  BOOST_CHECK (connections.size() == 2);

  for (auto it : connections) {
    BOOST_CHECK (it->getSource()->getId() == src->getId() );
  }

  g_object_set (src->getGstreamerElement(), "audio", TRUE, "video", TRUE, NULL);

  connections = src->getSinkConnections ();
  BOOST_CHECK (connections.size() == 2);

  for (auto it : connections) {
    BOOST_CHECK (it->getSource()->getId() == src->getId() );
  }

  connections = sink->getSourceConnections (AUDIO);
  BOOST_CHECK (connections.size() == 1);

  connections = sink->getSourceConnections (AUDIO, "");
  BOOST_CHECK (connections.size() == 1);

  connections = sink->getSourceConnections (AUDIO, "test");
  BOOST_CHECK (connections.size() == 0);

  connections = sink->getSourceConnections (VIDEO);
  BOOST_CHECK (connections.size() == 1);

  connections = sink->getSourceConnections (VIDEO, "");
  BOOST_CHECK (connections.size() == 1);

  connections = sink->getSourceConnections (VIDEO, "test");
  BOOST_CHECK (connections.size() == 0);

  src->disconnect (sink);

  connections = sink->getSourceConnections ();
  BOOST_CHECK (connections.size() == 0);

  src->connect (sink, AUDIO);

  connections = sink->getSourceConnections ();
  BOOST_CHECK (connections.size() == 1);

  connections = src->getSinkConnections ();
  BOOST_CHECK (connections.size() == 1);

  connections = sink->getSourceConnections (VIDEO, "");
  BOOST_CHECK (connections.size() == 0);

  src.reset();

  connections = sink->getSourceConnections ();
  BOOST_CHECK (connections.size() == 0);
}
