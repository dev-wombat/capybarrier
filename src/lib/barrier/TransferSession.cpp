/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "TransferSession.h"

#include "io/filesystem.h"
#include "barrier/protocol_types.h"

#include <openssl/evp.h>

#include <fstream>
#include <limits>
#include <system_error>

namespace {

bool
sha256File(const barrier::fs::path& path, std::string& value)
{
	std::ifstream file;
	barrier::open_utf8_path(file, path, std::ios::in | std::ios::binary);
	if (!file.is_open()) {
		return false;
	}

	EVP_MD_CTX* context = EVP_MD_CTX_create();
	if (context == NULL || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
		EVP_MD_CTX_destroy(context);
		return false;
	}

	char buffer[4096];
	while (file.good()) {
		file.read(buffer, sizeof(buffer));
		const std::streamsize count = file.gcount();
		if (count > 0 && EVP_DigestUpdate(context, buffer, static_cast<std::size_t>(count)) != 1) {
			EVP_MD_CTX_destroy(context);
			return false;
		}
	}
	if (file.bad()) {
		EVP_MD_CTX_destroy(context);
		return false;
	}

	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int length = 0;
	if (EVP_DigestFinal_ex(context, digest, &length) != 1) {
		EVP_MD_CTX_destroy(context);
		return false;
	}
	EVP_MD_CTX_destroy(context);

	static const char hex[] = "0123456789abcdef";
	value.clear();
	value.reserve(length * 2);
	for (unsigned int i = 0; i < length; ++i) {
		value.push_back(hex[digest[i] >> 4]);
		value.push_back(hex[digest[i] & 0x0f]);
	}
	return true;
}

} // namespace

bool
TransferSession::accept(const TransferManifest& manifest, const std::string& root)
{
	if (!m_root.empty() || root.empty() || manifest.entries().empty()) return false;
	std::error_code error;
	if (!barrier::fs::create_directory(root, error) || error) return false;
	m_root = root;
	m_entries = manifest.entries();
	m_offsets.assign(m_entries.size(), VerifiedOffset{0});
	m_finalized = false;
	barrier::fs::permissions(m_root, barrier::fs::perms::owner_all, error);
	if (error) { cancel(); return false; }
	for (std::size_t i = 0; i < m_entries.size(); ++i) {
		const barrier::fs::path path = barrier::fs::path(m_root) / m_entries[i].path;
		if (m_entries[i].isDirectory) {
			barrier::fs::create_directories(path, error);
		}
		else {
			barrier::fs::create_directories(path.parent_path(), error);
			if (!error) {
				std::ofstream file;
				barrier::open_utf8_path(file, path, std::ios::out | std::ios::binary);
				file.close();
				if (!file) { cancel(); return false; }
			}
		}
		if (error) { cancel(); return false; }
	}
	return true;
}

bool
TransferSession::resumes(const TransferManifest& manifest) const
{
	const std::vector<TransferManifest::Entry>& entries = manifest.entries();
	if (entries.size() != m_entries.size()) return false;
	for (std::size_t i = 0; i < entries.size(); ++i) {
		if (entries[i].isDirectory != m_entries[i].isDirectory ||
			entries[i].path != m_entries[i].path ||
			entries[i].size != m_entries[i].size ||
			entries[i].sha256 != m_entries[i].sha256) return false;
		if (!entries[i].isDirectory && !hasVerifiedFile(i)) return false;
	}
	return true;
}

bool
TransferSession::hasVerifiedFile(std::size_t entry) const
{
	std::error_code error;
	const auto size = barrier::fs::file_size(barrier::fs::path(m_root) / m_entries[entry].path, error);
	return !error && size == m_offsets[entry].offset;
}

bool
TransferSession::writeChunk(std::size_t entry, std::uint64_t offset, const std::string& data)
{
	if (m_finalized || entry >= m_entries.size() || m_entries[entry].isDirectory ||
		data.size() > PROTOCOL_MAX_STRING_LENGTH || offset > m_entries[entry].size ||
		offset > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)()) ||
		offset != m_offsets[entry].offset || data.size() > m_entries[entry].size - offset) {
		return false;
	}
	if (!hasVerifiedFile(entry)) return false;

	const barrier::fs::path path = barrier::fs::path(m_root) / m_entries[entry].path;
	std::fstream file;
	barrier::open_utf8_path(file, path, std::ios::in | std::ios::out | std::ios::binary);
	if (!file.is_open()) {
		return false;
	}
	file.seekp(static_cast<std::streamoff>(offset));
	file.write(data.data(), static_cast<std::streamsize>(data.size()));
	file.close();
	if (!file) {
		return false;
	}

	m_offsets[entry].offset += data.size();
	return true;
}

bool
TransferSession::finalize()
{
	if (m_root.empty() || m_entries.empty()) return false;
	for (std::size_t i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].isDirectory) {
			continue;
		}
		std::string digest;
		const barrier::fs::path path = barrier::fs::path(m_root) / m_entries[i].path;
		if (m_offsets[i].offset != m_entries[i].size || !sha256File(path, digest) ||
			digest != m_entries[i].sha256) {
			cancel();
			return false;
		}
	}
	m_finalized = true;
	return true;
}

bool
TransferSession::copyTo(const std::string& destination) const
{
	if (!m_finalized || destination.empty()) return false;
	const barrier::fs::path target(destination);
	std::error_code error;
	barrier::fs::path parent;
	for (const auto& component : barrier::fs::absolute(target, error)) {
		if (error) return false;
		parent /= component;
		const auto status = barrier::fs::symlink_status(parent, error);
		if (error && error != std::errc::no_such_file_or_directory) return false;
		if (barrier::fs::exists(status) && !barrier::fs::is_directory(status)) return false;
	}
	if (error && error != std::errc::no_such_file_or_directory) return false;
	for (const auto& entry : m_entries) {
		const auto top = *barrier::fs::path(entry.path).begin();
		const auto status = barrier::fs::symlink_status(target / top, error);
		if (barrier::fs::exists(status) || (error && error != std::errc::no_such_file_or_directory)) return false;
	}
	barrier::fs::create_directories(target, error);
	if (error) return false;
	for (std::size_t i = 0; i < m_entries.size(); ++i) {
		const barrier::fs::path source = barrier::fs::path(m_root) / m_entries[i].path;
		const barrier::fs::path output = target / m_entries[i].path;
		if (m_entries[i].isDirectory) {
			barrier::fs::create_directories(output, error);
			if (error) return false;
		}
		else {
			barrier::fs::create_directories(output.parent_path(), error);
			if (error || !barrier::fs::copy_file(source, output, barrier::fs::copy_options::none, error) || error) return false;
		}
	}
	return true;
}

void
TransferSession::cancel()
{
	std::error_code error;
	if (!m_root.empty()) barrier::fs::remove_all(m_root, error);
	m_root.clear();
	m_entries.clear();
	m_offsets.clear();
	m_finalized = false;
}

const std::vector<TransferSession::VerifiedOffset>&
TransferSession::verifiedOffsets() const
{
	return m_offsets;
}
