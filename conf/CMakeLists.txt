
# Copyright (C) Giuliano Catrambone (giuliano.catrambone@catrasoftware.it)

# This program is free software; you can redistribute it and/or 
# modify it under the terms of the GNU General Public License 
# as published by the Free Software Foundation; either 
# version 2 of the License, or (at your option) any later 
# version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

# Commercial use other than under the terms of the GNU General Public
# License is allowed only after express negotiation of conditions
# with the authors.

SET (CONFs
	nginx.conf
	catramms.nginx
	catrammsAPIServer.nginx
	catrammsGUIServer.nginx
	catrammsDeliveryServer.nginx
	catrammsDelivery-pathServer.nginx
	catrammsDelivery-fServer.nginx
	catrammsBinaryServer.nginx
	catrammsEncoderServer.nginx
	mms.cfg
	crontab.txt
	satellite_channels_hotbird_zap_2020_12_28.conf
	partitionInfo.json
)

install (FILES ${CONFs} DESTINATION conf)

