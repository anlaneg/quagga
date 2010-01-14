/* BGP Message Read Handling -- header
 * Copyright (C) 2010 Chris Hall (GMCH), Highwayman
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */


#ifndef BGP_MSG_READ_H_
#define BGP_MSG_READ_H_

#include "bgpd/bgp_common.h"

extern bgp_size_t
bgp_msg_get_mlen(uint8_t* p) ;

extern int
bgp_msg_check_header(bgp_connection connection);

extern void
bgp_msg_dispatch(bgp_connection connection);

#endif /* BGP_MSG_READ_H_ */
