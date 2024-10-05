/************************************************************************************
 *
 * D++, A Lightweight C++ library for Discord
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2021 Craig Edwards and D++ contributors
 * (https://github.com/brainboxdotcc/DPP/graphs/contributors)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This folder is a modified fork of libdave, https://github.com/discord/libdave
 * Copyright (c) 2024 Discord, Licensed under MIT
 *
 ************************************************************************************/
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <mutex>
#include <functional>
#include <iostream>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <fcntl.h>

#include <bytes/bytes.h>
#include <mls/crypto.h>

#include "parameters.h"
#include "logger.h"
#include "persisted_key_pair.h"

static const std::string_view KeyStorageDir = "Discord Key Storage";

static std::filesystem::path GetKeyStorageDirectory()
{
	std::filesystem::path dir;

#if defined(__ANDROID__)
	dir = std::filesystem::path("/data/data");

	{
		std::ifstream idFile("/proc/self/cmdline", std::ios_base::in);
		std::string appId;
		std::getline(idFile, appId, '\0');
		dir /= appId;
	}
#else // __ANDROID__
#if defined(_WIN32)
	if (const wchar_t* appdata = _wgetenv(L"LOCALAPPDATA")) {
		dir = std::filesystem::path(appdata);
	}
#else  // _WIN32
	if (const char* xdg = getenv("XDG_CONFIG_HOME")) {
		dir = std::filesystem::path(xdg);
	}
	else if (const char* home = getenv("HOME")) {
		dir = std::filesystem::path(home);
		dir /= ".config";
	}
#endif // !_WIN32
	else {
		return dir;
	}
#endif // !__ANDROID__

	return dir / KeyStorageDir;
}

namespace dpp::dave::mls::detail {

std::shared_ptr<::mlspp::SignaturePrivateKey> GetGenericPersistedKeyPair(KeyPairContextType ctx,
																		 const std::string& id,
																		 ::mlspp::CipherSuite suite)
{
	::mlspp::SignaturePrivateKey ret;
	std::string curstr;
	std::filesystem::path dir = GetKeyStorageDirectory();

	if (dir.empty()) {
		DISCORD_LOG(LS_ERROR) << "Failed to determine key storage directory in GetPersistedKeyPair";
		return nullptr;
	}

	std::error_code errc;
	std::filesystem::create_directories(dir, errc);
	if (errc) {
		DISCORD_LOG(LS_ERROR) << "Failed to create key storage directory in GetPersistedKeyPair: "
							  << errc;
		return nullptr;
	}

	std::filesystem::path file = dir / (id + ".key");

	if (std::filesystem::exists(file)) {
		std::ifstream ifs(file, std::ios_base::in | std::ios_base::binary);
		if (!ifs) {
			DISCORD_LOG(LS_ERROR) << "Failed to open key in GetPersistedKeyPair";
			return nullptr;
		}

	std::stringstream s;
	s << ifs.rdbuf();
	curstr = s.str();
		if (!ifs) {
			DISCORD_LOG(LS_ERROR) << "Failed to read key in GetPersistedKeyPair";
			return nullptr;
		}

		try {
			ret = ::mlspp::SignaturePrivateKey::from_jwk(suite, curstr);
		}
		catch (std::exception& ex) {
			DISCORD_LOG(LS_ERROR) << "Failed to parse key in GetPersistedKeyPair: " << ex.what();
			return nullptr;
		}
	}
	else {
		ret = ::mlspp::SignaturePrivateKey::generate(suite);

		std::string newstr = ret.to_jwk(suite);

		std::filesystem::path tmpfile = file;
		tmpfile += ".tmp";

#ifdef _WIN32
		int fd = _wopen(tmpfile.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC, _S_IREAD | _S_IWRITE);
#else
		int fd = open(tmpfile.c_str(),
					  O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_CREAT | O_TRUNC,
					  S_IRUSR | S_IWUSR);
#endif
		if (fd < 0) {
			DISCORD_LOG(LS_ERROR) << "Failed to open output file in GetPersistedKeyPair: " << errno
								  << "(" << tmpfile << ")";
			return nullptr;
		}

#ifdef _WIN32
		int wret = _write(fd, newstr.c_str(), newstr.size());
		_close(fd);
#else
		ssize_t wret = write(fd, newstr.c_str(), newstr.size());
		close(fd);
#endif
		if (wret < 0 || (size_t)wret != newstr.size()) {
			DISCORD_LOG(LS_ERROR) << "Failed to write output file in GetPersistedKeyPair: "
								  << errno;
			return nullptr;
		}

		std::filesystem::rename(tmpfile, file, errc);
		if (errc) {
			DISCORD_LOG(LS_ERROR) << "Failed to rename output file in GetPersistedKeyPair: "
								  << errc;
			return nullptr;
		}
	}

	if (!ret.public_key.data.empty()) {
		return std::make_shared<::mlspp::SignaturePrivateKey>(std::move(ret));
	}
	return nullptr;

}

bool DeleteGenericPersistedKeyPair(KeyPairContextType ctx, const std::string& id)
{
	std::error_code errc;
	std::filesystem::path dir = GetKeyStorageDirectory();
	if (dir.empty()) {
		DISCORD_LOG(LS_ERROR) << "Failed to determine key storage directory in GetPersistedKeyPair";
		return false;
	}

	std::filesystem::path file = dir / (id + ".key");

	return std::filesystem::remove(file, errc);
}

} // namespace dpp::dave::mls::detail



