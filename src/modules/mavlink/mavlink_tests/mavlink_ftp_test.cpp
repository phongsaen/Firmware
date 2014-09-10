/****************************************************************************
 *
 *   Copyright (C) 2014 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/// @file mavlink_ftp_test.cpp
///	@author Don Gagne <don@thegagnes.com>

#include <sys/stat.h>
#include <crc32.h>
#include <stdio.h>
#include <fcntl.h>

#include "mavlink_ftp_test.h"
#include "../mavlink_ftp.h"

/// @brief Test case file name for Read command. File are generated using mavlink_ftp_test_data.py
const MavlinkFtpTest::ReadTestCase MavlinkFtpTest::_rgReadTestCases[] = {
	{ "/etc/unit_test_data/mavlink_tests/test_238.data",	MAVLINK_MSG_FILE_TRANSFER_PROTOCOL_FIELD_PAYLOAD_LEN - sizeof(MavlinkFTP::PayloadHeader) - 1},	// Read takes less than single packet
	{ "/etc/unit_test_data/mavlink_tests/test_239.data",	MAVLINK_MSG_FILE_TRANSFER_PROTOCOL_FIELD_PAYLOAD_LEN - sizeof(MavlinkFTP::PayloadHeader) },		// Read completely fills single packet
	{ "/etc/unit_test_data/mavlink_tests/test_240.data",	MAVLINK_MSG_FILE_TRANSFER_PROTOCOL_FIELD_PAYLOAD_LEN - sizeof(MavlinkFTP::PayloadHeader) + 1 },	// Read take two packets
};

const char MavlinkFtpTest::_unittest_microsd_dir[] = "/fs/microsd/ftp_unit_test_dir";
const char MavlinkFtpTest::_unittest_microsd_file[] = "/fs/microsd/ftp_unit_test_dir/file";

MavlinkFtpTest::MavlinkFtpTest() :
	_ftp_server{},
	_reply_msg{},
	_lastOutgoingSeqNumber{}
{
}

MavlinkFtpTest::~MavlinkFtpTest()
{
	
}

/// @brief Called before every test to initialize the FTP Server.
void MavlinkFtpTest::init(void)
{
	_ftp_server = new MavlinkFTP;;
	_ftp_server->set_unittest_worker(MavlinkFtpTest::receive_message, this);

	_cleanup_microsd();
}

/// @brief Called after every test to take down the FTP Server.
void MavlinkFtpTest::cleanup(void)
{
	delete _ftp_server;
	
	_cleanup_microsd();
}

/// @brief Tests for correct behavior of an Ack response.
bool MavlinkFtpTest::_ack_test(void)
{
	MavlinkFTP::PayloadHeader		payload;
	mavlink_file_transfer_protocol_t	ftp_msg;
	MavlinkFTP::PayloadHeader		*reply;
	
	payload.opcode = MavlinkFTP::kCmdNone;

	bool success = _send_receive_msg(&payload,	// FTP payload header
					 0,		// size in bytes of data
					 nullptr,	// Data to start into FTP message payload
					 &ftp_msg,	// Response from server
					 &reply);	// Payload inside FTP message response
	if (!success) {
		return false;
	}
	
	ut_compare("Didn't get Ack back", reply->opcode, MavlinkFTP::kRspAck);
	ut_compare("Incorrect payload size", reply->size, 0);
	
	return true;
}

/// @brief Tests for correct response to an invalid opcpde.
bool MavlinkFtpTest::_bad_opcode_test(void)
{
	MavlinkFTP::PayloadHeader		payload;
	mavlink_file_transfer_protocol_t	ftp_msg;
	MavlinkFTP::PayloadHeader		*reply;
	
	payload.opcode = 0xFF;	// bogus opcode

	bool success = _send_receive_msg(&payload,	// FTP payload header
					 0,		// size in bytes of data
					 nullptr,	// Data to start into FTP message payload
					 &ftp_msg,	// Response from server
					 &reply);	// Payload inside FTP message response
	if (!success) {
		return false;
	}
	
	ut_compare("Didn't get Nak back", reply->opcode, MavlinkFTP::kRspNak);
	ut_compare("Incorrect payload size", reply->size, 1);
	ut_compare("Incorrect error code", reply->data[0], MavlinkFTP::kErrUnknownCommand);
	
	return true;
}

/// @brief Tests for correct reponse to a payload which an invalid data size field.
bool MavlinkFtpTest::_bad_datasize_test(void)
{
	mavlink_message_t			msg;
	MavlinkFTP::PayloadHeader		payload;
	mavlink_file_transfer_protocol_t	ftp_msg;
	MavlinkFTP::PayloadHeader		*reply;
	
	payload.opcode = MavlinkFTP::kCmdListDirectory;
	
	_setup_ftp_msg(&payload, 0, nullptr, &msg);
	
	// Set the data size to be one larger than is legal
	((MavlinkFTP::PayloadHeader*)((mavlink_file_transfer_protocol_t*)msg.payload64)->payload)->size = MAVLINK_MSG_FILE_TRANSFER_PROTOCOL_FIELD_PAYLOAD_LEN + 1;
	
	_ftp_server->handle_message(nullptr /* mavlink */, &msg);
	
	if (!_decode_message(&_reply_msg, &ftp_msg, &reply)) {
		return false;
	}
	
	ut_compare("Didn't get Nak back", reply->opcode, MavlinkFTP::kRspNak);
	ut_compare("Incorrect payload size", reply->size, 1);
	ut_compare("Incorrect error code", reply->data[0], MavlinkFTP::kErrInvalidDataSize);
	
	return true;
}

bool MavlinkFtpTest::_list_test(void)
{
	MavlinkFTP::PayloadHeader		payload;
	mavlink_file_transfer_protocol_t	ftp_msg;
	MavlinkFTP::PayloadHeader		*reply;
	
	char response1[] = "Dempty_dir|Ftest_238.data\t238|Ftest_239.data\t239|Ftest_240.data\t240";
	char response2[] = "Ddev|Detc|Dfs|Dobj";
	
	struct _testCase {
		const char	*dir;
		char		*response;
		bool		success;
	};
	struct _testCase rgTestCases[] = {
		{ "/bogus",				nullptr,	false },
		{ "/etc/unit_test_data/mavlink_tests",	response1,	true },
		{ "/",					response2,	true },
	};

	for (size_t i=0; i<sizeof(rgTestCases)/sizeof(rgTestCases[0]); i++) {
		const struct _testCase *test = &rgTestCases[i];
		
		uint8_t expected_data_size = strlen(test->response) + 1;
		
		char *ptr = strtok (test->response, "|");
		while (ptr != nullptr)
		{
			ptr = strtok (nullptr, "|");
		}

		payload.opcode = MavlinkFTP::kCmdListDirectory;
		payload.offset = 0;
		
		bool success = _send_receive_msg(&payload,		// FTP payload header
						strlen(test->dir)+1,	// size in bytes of data
						(uint8_t*)test->dir,	// Data to start into FTP message payload
						&ftp_msg,		// Response from server
						&reply);		// Payload inside FTP message response
		if (!success) {
			return false;
		}
		
		if (test->success) {
			ut_compare("Didn't get Ack back", reply->opcode, MavlinkFTP::kRspAck);
			ut_compare("Incorrect payload size", reply->size, expected_data_size);
			ut_compare("Ack payload contents incorrect", memcmp(reply->data, test->response, expected_data_size), 0);
		} else {
			ut_compare("Didn't get Nak back", reply->opcode, MavlinkFTP::kRspNak);
			ut_compare("Incorrect payload size", reply->size, 2);
			ut_compare("Incorrect error code", reply->data[0], MavlinkFTP::kErrFailErrno);
		}
	}
	
	return true;
}

/// @brief Tests for correct reponse to a List command on a valid directory, but with an offset that
/// is beyond the last directory entry.
bool MavlinkFtpTest::_list_eof_test(void)
{
	MavlinkFTP::PayloadHeader		payload;
	mavlink_file_transfer_protocol_t	ftp_msg;
	MavlinkFTP::PayloadHeader		*reply;
	const char				*dir = "/";
	
	payload.opcode = MavlinkFTP::kCmdListDirectory;
	payload.offset = 4;	// offset past top level dirs
	
	bool success = _send_receive_msg(&payload,	// FTP payload header
					strlen(dir)+1,	// size in bytes of data
					(uint8_t*)dir,	// Data to start into FTP message payload
					&ftp_msg,	// Response from server
					&reply);	// Payload inside FTP message response
	if (!success) {
		return false;
	}

	ut_compare("Didn't get Nak back", reply->opcode, MavlinkFTP::kRspNak);
	ut_compare("Incorrect payload size", reply->size, 1);
	ut_compare("Incorrect error code", reply->data[0], MavlinkFTP::kErrEOF);
	
	return true;
}

/// @brief Tests for correct reponse to an Open command on a file which does not exist.
bool MavlinkFtpTest::_open_badfile_test(void)
{
	MavlinkFTP::PayloadHeader		payload;
	mavlink_file_transfer_protocol_t	ftp_msg;
	MavlinkFTP::PayloadHeader		*reply;
	const char				*dir = "/foo";	// non-existent file
	
	payload.opcode = MavlinkFTP::kCmdOpenFile;
	payload.offset = 0;
	
	bool success = _send_receive_msg(&payload,	// FTP payload header
					strlen(dir)+1,	// size in bytes of data
					(uint8_t*)dir,	// Data to start into FTP message payload
					&ftp_msg,	// Response from server
					&reply);	// Payload inside FTP message response
	if (!success) {
		return false;
	}
	
	ut_compare("Didn't get Nak back", reply->opcode, MavlinkFTP::kRspNak);
	ut_compare("Incorrect payload size", reply->size, 2);
	ut_compare("Incorrect error code", reply->data[0], MavlinkFTP::kErrFailErrno);
	
	return true;
}

/// @brief Tests for correct reponse to an Open command on a file, followed by Terminate
bool MavlinkFtpTest::_open_terminate_test(void)
{
	MavlinkFTP::PayloadHeader		payload;
	mavlink_file_transfer_protocol_t	ftp_msg;
	MavlinkFTP::PayloadHeader		*reply;
	
	for (size_t i=0; i<sizeof(_rgReadTestCases)/sizeof(_rgReadTestCases[0]); i++) {
		struct stat st;
		const ReadTestCase *test = &_rgReadTestCases[i];
		
		payload.opcode = MavlinkFTP::kCmdOpenFile;
		payload.offset = 0;
		
		bool success = _send_receive_msg(&payload,		// FTP payload header
						 strlen(test->file)+1,	// size in bytes of data
						 (uint8_t*)test->file,	// Data to start into FTP message payload
						 &ftp_msg,		// Response from server
						 &reply);		// Payload inside FTP message response
		if (!success) {
			return false;
		}
		
		ut_compare("stat failed", stat(test->file, &st), 0);
		
		
		ut_compare("Didn't get Ack back", reply->opcode, MavlinkFTP::kRspAck);
		ut_compare("Incorrect payload size", reply->size, sizeof(uint32_t));
		ut_compare("File size incorrect", *((uint32_t*)&reply->data[0]), (uint32_t)st.st_size);

		payload.opcode = MavlinkFTP::kCmdTerminateSession;
		payload.session = reply->session;
		payload.size = 0;
		
		success = _send_receive_msg(&payload,	// FTP payload header
					    0,		// size in bytes of data
					    nullptr,	// Data to start into FTP message payload
					    &ftp_msg,	// Response from server
					    &reply);	// Payload inside FTP message response
		if (!success) {
			return false;
		}
		
		ut_compare("Didn't get Ack back", reply->opcode, MavlinkFTP::kRspAck);
		ut_compare("Incorrect payload size", reply->size, 0);
	}

	return true;
}

/// @brief Tests for correct reponse to a Terminate command on an invalid session.
bool MavlinkFtpTest::_terminate_badsession_test(void)
{
	MavlinkFTP::PayloadHeader		payload;
	mavlink_file_transfer_protocol_t	ftp_msg;
	MavlinkFTP::PayloadHeader		*reply;
	const char				*file = _rgReadTestCases[0].file;
	
	payload.opcode = MavlinkFTP::kCmdOpenFile;
	payload.offset = 0;
	
	bool success = _send_receive_msg(&payload,	// FTP payload header
					strlen(file)+1,	// size in bytes of data
					(uint8_t*)file,	// Data to start into FTP message payload
					&ftp_msg,	// Response from server
					&reply);	// Payload inside FTP message response
	if (!success) {
		return false;
	}
	
	ut_compare("Didn't get Ack back", reply->opcode, MavlinkFTP::kRspAck);
	
	payload.opcode = MavlinkFTP::kCmdTerminateSession;
	payload.session = reply->session + 1;
	payload.size = 0;
	
	success = _send_receive_msg(&payload,	// FTP payload header
				   0,		// size in bytes of data
				   nullptr,	// Data to start into FTP message payload
				   &ftp_msg,	// Response from server
				   &reply);	// Payload inside FTP message response
	if (!success) {
		return false;
	}
	
	ut_compare("Didn't get Nak back", reply->opcode, MavlinkFTP::kRspNak);
	ut_compare("Incorrect payload size", reply->size, 1);
	ut_compare("Incorrect error code", reply->data[0], MavlinkFTP::kErrInvalidSession);
	
	return true;
}

/// @brief Tests for correct reponse to a Read command on an open session.
bool MavlinkFtpTest::_read_test(void)
{
	MavlinkFTP::PayloadHeader		payload;
	mavlink_file_transfer_protocol_t	ftp_msg;
	MavlinkFTP::PayloadHeader		*reply;
	
	for (size_t i=0; i<sizeof(_rgReadTestCases)/sizeof(_rgReadTestCases[0]); i++) {
		struct stat st;
		const ReadTestCase *test = &_rgReadTestCases[i];
		
		// Read in the file so we can compare it to what we get back
		ut_compare("stat failed", stat(test->file, &st), 0);
		uint8_t *bytes = new uint8_t[st.st_size];
		ut_assert("new failed", bytes != nullptr);
		int fd = ::open(test->file, O_RDONLY);
		ut_assert("open failed", fd != -1);
		int bytes_read = ::read(fd, bytes, st.st_size);
		ut_compare("read failed", bytes_read, st.st_size);
		::close(fd);
		
		// Test case data files are created for specific boundary conditions
		ut_compare("Test case data files are out of date", test->length, st.st_size);
		
		payload.opcode = MavlinkFTP::kCmdOpenFile;
		payload.offset = 0;
		
		bool success = _send_receive_msg(&payload,		// FTP payload header
						 strlen(test->file)+1,	// size in bytes of data
						 (uint8_t*)test->file,	// Data to start into FTP message payload
						 &ftp_msg,		// Response from server
						 &reply);		// Payload inside FTP message response
		if (!success) {
			return false;
		}
		
		ut_compare("Didn't get Ack back", reply->opcode, MavlinkFTP::kRspAck);
		
		payload.opcode = MavlinkFTP::kCmdReadFile;
		payload.session = reply->session;
		payload.offset = 0;
		
		success = _send_receive_msg(&payload,	// FTP payload header
					    0,		// size in bytes of data
					    nullptr,	// Data to start into FTP message payload
					    &ftp_msg,	// Response from server
					    &reply);	// Payload inside FTP message response
		if (!success) {
			return false;
		}
		
		ut_compare("Didn't get Ack back", reply->opcode, MavlinkFTP::kRspAck);
		ut_compare("Offset incorrect", reply->offset, 0);
		
		if (test->length <= MAVLINK_MSG_FILE_TRANSFER_PROTOCOL_FIELD_PAYLOAD_LEN - sizeof(MavlinkFTP::PayloadHeader)) {
			ut_compare("Payload size incorrect", reply->size, (uint32_t)st.st_size);
			ut_compare("File contents differ", memcmp(reply->data, bytes, st.st_size), 0);
		}
		
		payload.opcode = MavlinkFTP::kCmdTerminateSession;
		payload.session = reply->session;
		payload.size = 0;
		
		success = _send_receive_msg(&payload,	// FTP payload header
					    0,		// size in bytes of data
					    nullptr,	// Data to start into FTP message payload
					    &ftp_msg,	// Response from server
					    &reply);	// Payload inside FTP message response
		if (!success) {
			return false;
		}
		
		ut_compare("Didn't get Ack back", reply->opcode, MavlinkFTP::kRspAck);
		ut_compare("Incorrect payload size", reply->size, 0);
	}
	
	return true;
}

/// @brief Tests for correct reponse to a Read command on an invalid session.
bool MavlinkFtpTest::_read_badsession_test(void)
{
	MavlinkFTP::PayloadHeader		payload;
	mavlink_file_transfer_protocol_t	ftp_msg;
	MavlinkFTP::PayloadHeader		*reply;
	const char				*file = _rgReadTestCases[0].file;
	
	payload.opcode = MavlinkFTP::kCmdOpenFile;
	payload.offset = 0;
	
	bool success = _send_receive_msg(&payload,	// FTP payload header
					 strlen(file)+1,	// size in bytes of data
					 (uint8_t*)file,	// Data to start into FTP message payload
					 &ftp_msg,	// Response from server
					 &reply);	// Payload inside FTP message response
	if (!success) {
		return false;
	}
	
	ut_compare("Didn't get Ack back", reply->opcode, MavlinkFTP::kRspAck);
	
	payload.opcode = MavlinkFTP::kCmdReadFile;
	payload.session = reply->session + 1;	// Invalid session
	payload.offset = 0;
	
	success = _send_receive_msg(&payload,	// FTP payload header
				    0,		// size in bytes of data
				    nullptr,	// Data to start into FTP message payload
				    &ftp_msg,	// Response from server
				    &reply);	// Payload inside FTP message response
	if (!success) {
		return false;
	}
	
	ut_compare("Didn't get Nak back", reply->opcode, MavlinkFTP::kRspNak);
	ut_compare("Incorrect payload size", reply->size, 1);
	ut_compare("Incorrect error code", reply->data[0], MavlinkFTP::kErrInvalidSession);
	
	return true;
}

bool MavlinkFtpTest::_removedirectory_test(void)
{
	MavlinkFTP::PayloadHeader		payload;
	mavlink_file_transfer_protocol_t	ftp_msg;
	MavlinkFTP::PayloadHeader		*reply;
	int					fd;
	
	struct _testCase {
		const char	*dir;
		bool		success;
		bool		deleteFile;
	};
	static const struct _testCase rgTestCases[] = {
		{ "/bogus",						false,	false },
		{ "/etc/unit_test_data/mavlink_tests/empty_dir",	false,	false },
		{ _unittest_microsd_dir,				false,	false },
		{ _unittest_microsd_file,				false,	false },
		{ _unittest_microsd_dir,				true,	true },
	};
	
	ut_compare("mkdir failed", ::mkdir(_unittest_microsd_dir, S_IRWXU | S_IRWXG | S_IRWXO), 0);
	ut_assert("open failed", (fd = ::open(_unittest_microsd_file, O_CREAT | O_EXCL)) != -1);
	::close(fd);
	
	for (size_t i=0; i<sizeof(rgTestCases)/sizeof(rgTestCases[0]); i++) {
		const struct _testCase *test = &rgTestCases[i];
		
		if (test->deleteFile) {
			ut_compare("unlink failed", ::unlink(_unittest_microsd_file), 0);
		}
		
		payload.opcode = MavlinkFTP::kCmdRemoveDirectory;
		payload.offset = 0;
		
		bool success = _send_receive_msg(&payload,		// FTP payload header
						 strlen(test->dir)+1,	// size in bytes of data
						 (uint8_t*)test->dir,	// Data to start into FTP message payload
						 &ftp_msg,		// Response from server
						 &reply);		// Payload inside FTP message response
		if (!success) {
			return false;
		}
		
		if (test->success) {
			ut_compare("Didn't get Ack back", reply->opcode, MavlinkFTP::kRspAck);
			ut_compare("Incorrect payload size", reply->size, 0);
		} else {
			ut_compare("Didn't get Nak back", reply->opcode, MavlinkFTP::kRspNak);
			ut_compare("Incorrect payload size", reply->size, 2);
			ut_compare("Incorrect error code", reply->data[0], MavlinkFTP::kErrFailErrno);
		}
	}
	
	return true;
}

bool MavlinkFtpTest::_createdirectory_test(void)
{
	MavlinkFTP::PayloadHeader		payload;
	mavlink_file_transfer_protocol_t	ftp_msg;
	MavlinkFTP::PayloadHeader		*reply;
	
	struct _testCase {
		const char	*dir;
		bool		success;
	};
	static const struct _testCase rgTestCases[] = {
		{ "/etc/bogus",			false },
		{ _unittest_microsd_dir,	true },
		{ _unittest_microsd_dir,	false },
		{ "/fs/microsd/bogus/bogus",	false },
	};
	
	for (size_t i=0; i<sizeof(rgTestCases)/sizeof(rgTestCases[0]); i++) {
		const struct _testCase *test = &rgTestCases[i];
		
		payload.opcode = MavlinkFTP::kCmdCreateDirectory;
		payload.offset = 0;
		
		bool success = _send_receive_msg(&payload,		// FTP payload header
						 strlen(test->dir)+1,	// size in bytes of data
						 (uint8_t*)test->dir,	// Data to start into FTP message payload
						 &ftp_msg,		// Response from server
						 &reply);		// Payload inside FTP message response
		if (!success) {
			return false;
		}
		
		if (test->success) {
			ut_compare("Didn't get Ack back", reply->opcode, MavlinkFTP::kRspAck);
			ut_compare("Incorrect payload size", reply->size, 0);
		} else {
			ut_compare("Didn't get Nak back", reply->opcode, MavlinkFTP::kRspNak);
			ut_compare("Incorrect payload size", reply->size, 2);
			ut_compare("Incorrect error code", reply->data[0], MavlinkFTP::kErrFailErrno);
		}
	}
	
	return true;
}

bool MavlinkFtpTest::_removefile_test(void)
{
	MavlinkFTP::PayloadHeader		payload;
	mavlink_file_transfer_protocol_t	ftp_msg;
	MavlinkFTP::PayloadHeader		*reply;
	int					fd;
	
	struct _testCase {
		const char	*file;
		bool		success;
	};
	static const struct _testCase rgTestCases[] = {
		{ "/bogus",			false },
		{ _rgReadTestCases[0].file,	false },
		{ _unittest_microsd_dir,	false },
		{ _unittest_microsd_file,	true },
		{ _unittest_microsd_file,	false },
	};
	
	ut_compare("mkdir failed", ::mkdir(_unittest_microsd_dir, S_IRWXU | S_IRWXG | S_IRWXO), 0);
	ut_assert("open failed", (fd = ::open(_unittest_microsd_file, O_CREAT | O_EXCL)) != -1);
	::close(fd);
	
	for (size_t i=0; i<sizeof(rgTestCases)/sizeof(rgTestCases[0]); i++) {
		const struct _testCase *test = &rgTestCases[i];
		
		payload.opcode = MavlinkFTP::kCmdRemoveFile;
		payload.offset = 0;
		
		bool success = _send_receive_msg(&payload,		// FTP payload header
						 strlen(test->file)+1,	// size in bytes of data
						 (uint8_t*)test->file,	// Data to start into FTP message payload
						 &ftp_msg,		// Response from server
						 &reply);		// Payload inside FTP message response
		if (!success) {
			return false;
		}
		
		if (test->success) {
			ut_compare("Didn't get Ack back", reply->opcode, MavlinkFTP::kRspAck);
			ut_compare("Incorrect payload size", reply->size, 0);
		} else {
			ut_compare("Didn't get Nak back", reply->opcode, MavlinkFTP::kRspNak);
			ut_compare("Incorrect payload size", reply->size, 2);
			ut_compare("Incorrect error code", reply->data[0], MavlinkFTP::kErrFailErrno);
		}
	}
	
	return true;
}

/// @brief Static method used as callback from MavlinkFTP. This method will be called by MavlinkFTP when
/// it needs to send a message out on Mavlink.
void MavlinkFtpTest::receive_message(const mavlink_message_t *msg, MavlinkFtpTest *ftp_test)
{
	ftp_test->_receive_message(msg);
}

/// @brief Non-Static version of receive_message
void MavlinkFtpTest::_receive_message(const mavlink_message_t *msg)
{
	// Move the message into our own member variable
	memcpy(&_reply_msg, msg, sizeof(mavlink_message_t));
}

/// @brief Decode and validate the incoming message
bool MavlinkFtpTest::_decode_message(const mavlink_message_t		*msg,		///< Mavlink message to decode
				     mavlink_file_transfer_protocol_t	*ftp_msg,	///< Decoded FTP message
				     MavlinkFTP::PayloadHeader		**payload)	///< Payload inside FTP message response
{
	mavlink_msg_file_transfer_protocol_decode(msg, ftp_msg);
	
	// Make sure the targets are correct
	ut_compare("Target network non-zero", ftp_msg->target_network, 0);
	ut_compare("Target system id mismatch", ftp_msg->target_system, clientSystemId);
	ut_compare("Target component id mismatch", ftp_msg->target_component, clientComponentId);
	
	*payload = reinterpret_cast<MavlinkFTP::PayloadHeader *>(ftp_msg->payload);
	
	// Make sure we have a good sequence number
	ut_compare("Sequence number mismatch", (*payload)->seqNumber, _lastOutgoingSeqNumber + 1);
	
	// Bump sequence number for next outgoing message
	_lastOutgoingSeqNumber++;
	
	return true;
}

/// @brief Initializes an FTP message into a mavlink message
void MavlinkFtpTest::_setup_ftp_msg(MavlinkFTP::PayloadHeader	*payload_header,	///< FTP payload header
				    uint8_t			size,			///< size in bytes of data
				    const uint8_t		*data,			///< Data to start into FTP message payload
				    mavlink_message_t		*msg)			///< Returned mavlink message
{
	uint8_t payload_bytes[MAVLINK_MSG_FILE_TRANSFER_PROTOCOL_FIELD_PAYLOAD_LEN];
	MavlinkFTP::PayloadHeader *payload = reinterpret_cast<MavlinkFTP::PayloadHeader *>(payload_bytes);
			     
	memcpy(payload, payload_header, sizeof(MavlinkFTP::PayloadHeader));
	
	payload->seqNumber = _lastOutgoingSeqNumber;
	payload->size = size;
	if (size != 0) {
		memcpy(payload->data, data, size);
	}
    
	payload->padding[0] = 0;
	payload->padding[1] = 0;
	
	msg->checksum = 0;
	mavlink_msg_file_transfer_protocol_pack(clientSystemId,		// Sender system id
						clientComponentId,	// Sender component id
						msg,			// Message to pack payload into
						0,			// Target network
						serverSystemId,		// Target system id
						serverComponentId,	// Target component id
						payload_bytes);		// Payload to pack into message
}

/// @brief Sends the specified FTP message to the server and returns response
bool MavlinkFtpTest::_send_receive_msg(MavlinkFTP::PayloadHeader	*payload_header,	///< FTP payload header
				      uint8_t				size,			///< size in bytes of data
				      const uint8_t			*data,			///< Data to start into FTP message payload
				      mavlink_file_transfer_protocol_t	*ftp_msg_reply,		///< Response from server
				      MavlinkFTP::PayloadHeader		**payload_reply)	///< Payload inside FTP message response
{
	mavlink_message_t msg;

	_setup_ftp_msg(payload_header, size, data, &msg);
	_ftp_server->handle_message(nullptr /* mavlink */, &msg);
	return _decode_message(&_reply_msg, ftp_msg_reply, payload_reply);
}
	
/// @brief Cleans up an files created on microsd during testing
void MavlinkFtpTest::_cleanup_microsd(void)
{
	::unlink(_unittest_microsd_file);
	::rmdir(_unittest_microsd_dir);
}

/// @brief Runs all the unit tests
void MavlinkFtpTest::runTests(void)
{
	ut_run_test(_ack_test);
	ut_run_test(_bad_opcode_test);
	ut_run_test(_bad_datasize_test);
	ut_run_test(_list_test);
	ut_run_test(_list_eof_test);
	ut_run_test(_open_badfile_test);
	ut_run_test(_open_terminate_test);
	ut_run_test(_terminate_badsession_test);
	ut_run_test(_read_test);
	ut_run_test(_read_badsession_test);
	ut_run_test(_removedirectory_test);
	ut_run_test(_createdirectory_test);
	ut_run_test(_removefile_test);
}

