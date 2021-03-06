/*****************************************************************************
 *                                McPAT
 *                      SOFTWARE LICENSE AGREEMENT
 *            Copyright 2009 Hewlett-Packard Development Company, L.P.
 *                          All Rights Reserved
 *
 * Permission to use, copy, and modify this software and its documentation is
 * hereby granted only under the following terms and conditions.  Both the
 * above copyright notice and this permission notice must appear in all copies
 * of the software, derivative works or modified versions, and any portions
 * thereof, and both notices must appear in supporting documentation.
 *
 * Any User of the software ("User"), by accessing and using it, agrees to the
 * terms and conditions set forth herein, and hereby grants back to Hewlett-
 * Packard Development Company, L.P. and its affiliated companies ("HP") a
 * non-exclusive, unrestricted, royalty-free right and license to copy,
 * modify, distribute copies, create derivate works and publicly display and
 * use, any changes, modifications, enhancements or extensions made to the
 * software by User, including but not limited to those affording
 * compatibility with other hardware or software, but excluding pre-existing
 * software applications that may incorporate the software.  User further
 * agrees to use its best efforts to inform HP of any such changes,
 * modifications, enhancements or extensions.
 *
 * Correspondence should be provided to HP at:
 *
 * Director of Intellectual Property Licensing
 * Office of Strategy and Technology
 * Hewlett-Packard Company
 * 1501 Page Mill Road
 * Palo Alto, California  94304
 *
 * The software may be further distributed by User (but not offered for
 * sale or transferred for compensation) to third parties, under the
 * condition that such third parties agree to abide by the terms and
 * conditions of this license.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" WITH ANY AND ALL ERRORS AND DEFECTS
 * AND USER ACKNOWLEDGES THAT THE SOFTWARE MAY CONTAIN ERRORS AND DEFECTS.
 * HP DISCLAIMS ALL WARRANTIES WITH REGARD TO THE SOFTWARE, INCLUDING ALL
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS.   IN NO EVENT SHALL
 * HP BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES
 * OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
 * WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THE SOFTWARE.
 *
 ***************************************************************************/

#include <time.h>
#include <math.h>


#include "area.h"
#include "basic_circuit.h"
#include "component.h"
#include "const.h"
#include "parameter.h"
#include "cacti_interface.h"
#include "Ucache.h"

#include <pthread.h>
#include <iostream>
#include <algorithm>

using namespace std;


bool mem_array::lt(const mem_array * m1, const mem_array * m2)
{
  if (m1->Nspd < m2->Nspd) return true;
  else if (m1->Nspd > m2->Nspd) return false;
  else if (m1->Ndwl < m2->Ndwl) return true;
  else if (m1->Ndwl > m2->Ndwl) return false;
  else if (m1->Ndbl < m2->Ndbl) return true;
  else if (m1->Ndbl > m2->Ndbl) return false;
  else if (m1->deg_bl_muxing < m2->deg_bl_muxing) return true;
  else if (m1->deg_bl_muxing > m2->deg_bl_muxing) return false;
  else if (m1->Ndsam_lev_1 < m2->Ndsam_lev_1) return true;
  else if (m1->Ndsam_lev_1 > m2->Ndsam_lev_1) return false;
  else if (m1->Ndsam_lev_2 < m2->Ndsam_lev_2) return true;
  else return false;
}



void uca_org_t::find_delay()
{
  mem_array * data_arr = data_array2;
  mem_array * tag_arr  = tag_array2;

  // check whether it is a regular cache or scratch ram
  if (g_ip->pure_ram|| g_ip->pure_cam || g_ip->fully_assoc)
  {
    access_time = data_arr->access_time;
  }
  // Both tag and data lookup happen in parallel
  // and the entire set is sent over the data array h-tree without
  // waiting for the way-select signal --TODO add the corresponding
  // power overhead Nav
  else if (g_ip->fast_access == true)
  {
    access_time = MAX(tag_arr->access_time, data_arr->access_time);
  }
  // Tag is accessed first. On a hit, way-select signal along with the
  // address is sent to read/write the appropriate block in the data
  // array
  else if (g_ip->is_seq_acc == true)
  {
    access_time = tag_arr->access_time + data_arr->access_time;
  }
  // Normal access: tag array access and data array access happen in parallel.
  // But, the data array will wait for the way-select and transfer only the
  // appropriate block over the h-tree.
  else
  {
    access_time = MAX(tag_arr->access_time + data_arr->delay_senseamp_mux_decoder,
                      data_arr->delay_before_subarray_output_driver) +
                  data_arr->delay_from_subarray_output_driver_to_output;
  }
}



void uca_org_t::find_energy()
{
  if (!(g_ip->pure_ram|| g_ip->pure_cam || g_ip->fully_assoc))//(g_ip->is_cache)
    power = data_array2->power + tag_array2->power;
  else
    power = data_array2->power;
}



void uca_org_t::find_area()
{
  if (g_ip->pure_ram|| g_ip->pure_cam || g_ip->fully_assoc)//(g_ip->is_cache == false)
  {
    cache_ht  = data_array2->height;
    cache_len = data_array2->width;
  }
  else
  {
    cache_ht  = MAX(tag_array2->height, data_array2->height);
    cache_len = tag_array2->width + data_array2->width;
  }
  area = cache_ht * cache_len;
}



void uca_org_t::find_cyc()
{
  if ((g_ip->pure_ram|| g_ip->pure_cam || g_ip->fully_assoc))//(g_ip->is_cache == false)
  {
    cycle_time = data_array2->cycle_time;
  }
  else
  {
    cycle_time = MAX(tag_array2->cycle_time,
                    data_array2->cycle_time);
  }
}

void uca_org_t :: cleanup()
{
	//	uca_org_t * it_uca_org;
	if (data_array2!=0){
		delete data_array2;
		data_array2 =0;
	}

	if (tag_array2!=0){
		delete tag_array2;
		tag_array2 =0;
	}

	std::vector<uca_org_t * >::size_type sz = uca_q.size();
	for (int i=sz-1; i>=0; i--)
	{
		if (uca_q[i]->data_array2!=0)
		{
			delete uca_q[i]->data_array2;
			uca_q[i]->data_array2 =0;
		}
		if (uca_q[i]->tag_array2!=0){
			delete uca_q[i]->tag_array2;
			uca_q[i]->tag_array2 =0;
		}
		delete uca_q[i];
		uca_q[i] =0;
		uca_q.pop_back();
	}

	if (uca_pg_reference!=0)
	{
		if (uca_pg_reference->data_array2!=0)
		{
			delete uca_pg_reference->data_array2;
			uca_pg_reference->data_array2 =0;
		}
		if (uca_pg_reference->tag_array2!=0){
			delete uca_pg_reference->tag_array2;
			uca_pg_reference->tag_array2 =0;
		}
		delete uca_pg_reference;
		uca_pg_reference =0;
	}
}


