/**
 * \file fastbit_table.cpp
 * \author Petr Kramolis <kramolis@cesnet.cz>
 * \brief methods of object wrapers for information elements.
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

extern "C" {
#include <ipfixcol/verbose.h>
}

#include <vector>

#include "fastbit_table.h"

#define ROW_LINE "Number_of_rows ="

uint64_t get_rows_from_part(const char *part_path)
{
	uint64_t rows = 0;
	std::string line;
	std::string str_row;
	size_t pos;

	std::ifstream part_file(part_path);
	if (!part_file.is_open()){
		return rows;
	}

	while (getline(part_file, line, '\n')) {
		if ((pos = line.find(ROW_LINE)) != std::string::npos){
			str_row = line.substr(pos + strlen(ROW_LINE));
			rows = strtoul(str_row.c_str(), NULL, 0);
		}
	}

	return rows;
}

template_table::template_table(uint16_t template_id, uint32_t buff_size): _rows_count(0)
{
	_template_id = template_id;
	sprintf(_name, "%u",template_id);
	_orig_name[0] = '\0'; /* Empty string indicates that we use original name from template_id */
	_index = 0;
	_rows_in_window = 0;
	_min_record_size = 0;
	_new_dir = true;

	if (buff_size == 0){
		buff_size = RESERVED_SPACE;
	}

	_buff_size = buff_size;
	_first_transmission = 0;
}

template_table::~template_table()
{
	for (el_it = elements.begin(); el_it != elements.end(); ++el_it) {
		delete (*el_it);
	}
}

int template_table::update_part(std::string path)
{
	FILE *f;
	std::stringstream ss;
	std::string part;
	uint64_t rows_in_part;
	rows_in_part = get_rows_from_part((path + "/-part.txt").c_str());

	f = fopen((path + "/-part.txt").c_str(), "w");
	if (f == NULL){
		MSG_ERROR(msg_module, "Cannot open/update -part.txt file");
		return 1;
	}

	/* Insert header */
	ss << "BEGIN HEADER";
	ss << "\nName = " << std::string(this->_name);
	ss << "\nDescription = Generated by FastBit plugin for IPFIXcol";
	ss << "\nNumber_of_rows = " << _rows_in_window + rows_in_part;
	ss << "\nNumber_of_columns = " << elements.size();
	ss << "\nTimestamp = " << time(NULL);
	ss << "\nEND HEADER\n\n";

	/* Insert row info */
	for (el_it = elements.begin(); el_it != elements.end(); ++el_it){
		ss << (*el_it)->get_part_info();
	}

	part = ss.str();
	fputs(part.c_str(), f);
	fclose(f);
	return 0;
}

int template_table::dir_check(std::string path, bool new_dir)
{
	size_t pos;

	/* Creating directory, unset the _new_dir property */
	this->_new_dir = false;

	if (mkdir(path.c_str(), 0777) != 0) {
		if (errno == EEXIST) { /* dir already exists */
			if (new_dir) {
				/* Rename the table */
				int len = strlen(this->_name);

				if (this->_orig_name[0] == '\0') {
					/* Save the _name property if not saved already */
					strcpy(this->_orig_name, this->_name);

					/* Update the name */
					this->_name[len] = 'a';
					this->_name[len + 1] = '\0';

					/* Update the path */
					path += "a";
				} else {
					/* Update the name */
					this->_name[len - 1]++;

					/* Update the path */
					path[path.length() - 1]++;
				}

				return this->dir_check(path.c_str(), true);
			}

			return 0;
		}

		if (errno == ENOENT) { /* Check parent directory */
			pos = path.find_last_of("/\\");
			if (pos == std::string::npos) {
				MSG_ERROR(msg_module, "Cannot create directory %s", path.c_str());
				return 2;
			}

			/* Create the parent */
			this->dir_check(path.substr(0,pos), false);

			/* Try to create the dir again */
			if (mkdir(path.c_str(), 0777) != 0){
				MSG_ERROR(msg_module, "Cannot create directory %s", path.c_str());
				return 2;
			}

			return 0;
		}

		/* Other error */
		MSG_ERROR(msg_module, "Cannot create directory %s", path.c_str());
		return 2;
	}
	
	return 0;
}

int template_table::store(ipfix_data_set *data_set, std::string path, bool new_dir)
{
	uint8_t *data = data_set->records;
	uint16_t element_size = 0;
	uint32_t record_cnt = 0;

	if (data == NULL) {
		return 0;
	}

	/* When opening new directory, go back to original name (duplicity should be gone) */
	if (new_dir && this->_orig_name[0] != '\0') {
		if (this->_rows_in_window > this->_rows_count) {
			MSG_ERROR(msg_module, "Renaming partially stored template");
		}

		strcpy(this->_name, this->_orig_name);
		this->_orig_name[0] = '\0';
	}

	if (new_dir) {
		this->_new_dir = true;
	}

	/* Count how many records does data_set contain */
	uint16_t data_size = (ntohs(data_set->header.length) - (sizeof(struct ipfix_set_header)));
	uint16_t read_data = 0;
	while (read_data < data_size) {
		if ((data_size - read_data) < _min_record_size) {
			break;
		}

		record_cnt++;

		for (el_it = elements.begin(); el_it != elements.end() && read_data < data_size; ++el_it) {
			element_size = (*el_it)->fill(data);
			data += element_size;
			read_data += element_size;
		}

		_rows_count++;
		if (_rows_count >= _buff_size) {
			_rows_in_window += _rows_count;

			if (this->dir_check(path + _name, this->_new_dir) != 0) {
				return -1;
			}

			for (el_it = elements.begin(); el_it != elements.end(); ++el_it) {
				(*el_it)->flush(path + _name);
			}

			/* Update -part.txt so that the data is ready for processing */
			this->update_part(path + _name);
			_rows_count = 0;
			_rows_in_window = 0;
		}
	}

	return record_cnt;
}

void template_table::flush(std::string path)
{
	/* Check whether there is something to flush */
	if (_rows_count <= 0) {
		return;
	}

	/* Check directory */
	_rows_in_window += _rows_count;
	if (this->dir_check(path + _name, this->_new_dir) != 0) {
		return;
	}

	/* Flush data */
	for (el_it = elements.begin(); el_it != elements.end(); ++el_it) {
		(*el_it)->flush(path + _name);
	}

	/* Create/update -part.txt file */
	this->update_part(path + _name);
	_rows_count = 0;
	_rows_in_window = 0;

	/* Data on disk is consistent; try to go back to original name */
	if (this->_orig_name[0] != '\0') {
		strcpy(this->_name, this->_orig_name);
		this->_orig_name[0] = '\0';
	}
}

int template_table::parse_template(struct ipfix_template *tmp,struct fastbit_config *config)
{
	int i;
	uint32_t en = 0; /* enterprise number (0 = IANA elements) */
	int en_offset = 0;
	template_ie *field;
	element *new_element;

	/* Is there anything to parse? */
	if (tmp == NULL){
		MSG_WARNING(msg_module, "Received data without template, skipping");
		return 1;
	}

	/* We don't want to parse option tables ect., so check it */
	if (tmp->template_type != TM_TEMPLATE){
		MSG_WARNING(msg_module, "Received Options Template; skipping data...");
		return 1; 
	}

	_template_id = tmp->template_id;

	/* Save the time of the template transmission */
	_first_transmission = tmp->first_transmission;

	/* Find elements */
	for (i = 0; i < tmp->field_count + en_offset; i++){
		field = &(tmp->fields[i]);
		if (field->ie.length == VAR_IE_LENGTH){
			_min_record_size += 1;
		} else {
			_min_record_size += field->ie.length;
		}
		
		/* Is this an enterprise element? */
		en = 0;
		if (field->ie.id & 0x8000){
			i++;
			en_offset++;
			en = tmp->fields[i].enterprise_number;
		}

		switch(get_type_from_xml(config, en, field->ie.id & 0x7FFF)){
			case UINT:
				new_element = new el_uint(field->ie.length, en, field->ie.id & 0x7FFF, _buff_size);
				break;
			case IPv6:
				/* IPv6 address are 128b in size, so we have to split them over two 64b rows
				 * Adding p0 p1 sufixes to row name...
				 */
				/* Check size from template */
				if (field->ie.length != 16) {
					MSG_WARNING(msg_module, "Element e%iid%i has type IPv6 but size %i; skipping...", en, field->ie.id & 0x7FFF, field->ie.length);
					new_element = new el_unknown(field->ie.length);
					break;
				}
				new_element = new el_ipv6(sizeof(uint64_t), en, field->ie.id & 0x7FFF, 0, _buff_size);
				elements.push_back(new_element);

				new_element = new el_ipv6(sizeof(uint64_t), en, field->ie.id & 0x7FFF, 1, _buff_size);
				break;
			case INT: 
				new_element = new el_sint(field->ie.length, en, field->ie.id & 0x7FFF, _buff_size);
				break;
			case FLOAT:
				new_element = new el_float(field->ie.length, en, field->ie.id & 0x7FFF, _buff_size);
				break;
			case TEXT:
				new_element = new el_text(field->ie.length, en, field->ie.id & 0x7FFF, _buff_size);
				break;
			case BLOB:
				new_element = new el_blob(field->ie.length, en, field->ie.id & 0x7FFF, _buff_size);
				break;
			case UNKNOWN:
			default:
				MSG_DEBUG(msg_module, "Received UNKNOWN element (size: %u)",field->ie.length);
				if (field->ie.length < 9){
					new_element = new el_uint(field->ie.length, en, field->ie.id & 0x7FFF, _buff_size);
				} else if (field->ie.length == VAR_IE_LENGTH){ /* Variable size element */
					new_element = new el_var_size(field->ie.length, en, field->ie.id & 0x7FFF, _buff_size);
				} else { /* TODO blob etc. */
					new_element = new el_blob(field->ie.length, en, field->ie.id & 0x7FFF, _buff_size);
				}

				break;
		}

		if (!new_element){
			MSG_ERROR(msg_module, "Something is wrong with template elements...");
			return 1;
		}

		elements.push_back(new_element);
	}

	return 0;
}
