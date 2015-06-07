/**
 * \file Profile.h
 * \author Michal Kozubik <kozubik@cesnet.cz>
 * \brief
 *
 * Copyright (C) 2015 CESNET, z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is, and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */

#ifndef PROFILE_H
#define	PROFILE_H

#include <string>
#include <vector>

#include "Channel.h"
#include "pugixml/pugixml.hpp"

class Channel;

/**
 * \brief Class representing profile
 */
class Profile {
public:
	/* Shortcuts */
	using channelsVec = std::vector<Channel *>;
	using profilesVec = std::vector<Profile *>;

	Profile(std::string name);
	~Profile();

	void destroy();

	void setNode(pugi::xml_node node);
	void setParent(Profile *parent) { m_parent = parent; }

	void addProfile(Profile *child, bool loadingXml = false);
	void removeProfile(Profile *child);

	void removeChannel(Channel *channel);
	void addChannel(Channel *channel, bool loadingXml = false);

	Profile *getParent() { return m_parent; }
	std::string getName() { return m_name; }
	std::string getPathName() { return m_pathName; }
	pugi::xml_node getNode() { return m_node; }

	channelsVec getChannels() { return m_channels; }
	profilesVec getChildren() { return m_children; }

	void updatePathName();
	void updateNodeData();

private:

	Profile *m_parent{NULL};	/**< Parent profile */

	std::string m_pathName{};	/**< rootName/../parentName/myName */
	std::string m_name{};		/**< Profile name */
	profilesVec m_children{};	/**< Children */
	channelsVec m_channels{};	/**< Channels */
	pugi::xml_node m_node;
};

#endif	/* PROFILE_H */

