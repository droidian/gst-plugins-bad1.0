/* GStreamer
 * Copyright (C) 2022 Seungha Yang <seungha@centricular.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 */

#include "win32ipcpipeclient.h"
#include "win32ipcutils.h"
#include <mutex>
#include <condition_variable>
#include <memory>
#include <thread>
#include <queue>
#include <string>

GST_DEBUG_CATEGORY_EXTERN (gst_win32_ipc_debug);
#define GST_CAT_DEFAULT gst_win32_ipc_debug

#define CONN_BUFFER_SIZE 1024

struct MmfInfo
{
  Win32IpcMmf *mmf;
  Win32IpcVideoInfo info;
};

struct ClientConnection : public OVERLAPPED
{
  ClientConnection () : pipe (INVALID_HANDLE_VALUE), to_read (0), to_write (0),
      seq_num (0)
  {
    OVERLAPPED *parent = dynamic_cast<OVERLAPPED *> (this);
    parent->Internal = 0;
    parent->InternalHigh = 0;
    parent->Offset = 0;
    parent->OffsetHigh = 0;
  }

  Win32IpcPipeClient *self;
  HANDLE pipe;
  UINT8 client_msg[CONN_BUFFER_SIZE];
  UINT32 to_read;
  UINT8 server_msg[CONN_BUFFER_SIZE];
  UINT32 to_write;
  UINT64 seq_num;
};

struct Win32IpcPipeClient
{
  explicit Win32IpcPipeClient (const std::string & n)
    : name (n), ref_count(1), last_err (ERROR_SUCCESS)
  {
    cancellable = CreateEventA (nullptr, TRUE, FALSE, nullptr);
    conn.pipe = INVALID_HANDLE_VALUE;
    conn.self = this;
  }

  ~Win32IpcPipeClient ()
  {
    GST_DEBUG ("Free client %p", this);
    win32_ipc_pipe_client_shutdown (this);
    CloseHandle (cancellable);
  }

  std::mutex lock;
  std::condition_variable cond;
  std::unique_ptr<std::thread> thread;
  std::queue<MmfInfo> queue;
  std::string name;

  ULONG ref_count;
  HANDLE cancellable;
  UINT last_err;
  ClientConnection conn;
};

static DWORD
win32_ipc_pipe_client_send_need_data_async (Win32IpcPipeClient * self);

static VOID WINAPI
win32_ipc_pipe_client_send_read_done_finish (DWORD error_code, DWORD n_bytes,
    LPOVERLAPPED overlapped)
{
  ClientConnection *conn = (ClientConnection *) overlapped;
  Win32IpcPipeClient *self = conn->self;

  if (error_code != ERROR_SUCCESS) {
    std::string msg = win32_ipc_error_message (error_code);
    self->last_err = error_code;
    GST_WARNING ("READ-DONE failed with 0x%x (%s)",
        self->last_err, msg.c_str ());
    goto error;
  }

  GST_TRACE ("READ-DONE sent");

  self->last_err = win32_ipc_pipe_client_send_need_data_async (self);
  if (self->last_err != ERROR_SUCCESS)
    goto error;

  /* All done, back to need-data state */
  return;

error:
  SetEvent (self->cancellable);
}

static DWORD
win32_ipc_pipe_client_send_read_done_async (Win32IpcPipeClient * self)
{
  ClientConnection *conn = &self->conn;

  conn->to_write = win32_ipc_pkt_build_read_done (conn->client_msg,
      CONN_BUFFER_SIZE, conn->seq_num);
  if (conn->to_write == 0) {
    GST_ERROR ("Couldn't build READ-DONE pkt");
    return ERROR_BAD_FORMAT;
  }

  GST_TRACE ("Sending READ-DONE");

  if (!WriteFileEx (conn->pipe, conn->client_msg, conn->to_write,
      (OVERLAPPED *) conn, win32_ipc_pipe_client_send_read_done_finish)) {
    UINT last_err = GetLastError ();
    std::string msg = win32_ipc_error_message (last_err);

    GST_WARNING ("WriteFileEx failed with 0x%x (%s)", last_err, msg.c_str ());
    return last_err;
  }

  return ERROR_SUCCESS;
}

static VOID WINAPI
win32_ipc_pipe_client_receive_have_data_finish (DWORD error_code, DWORD n_bytes,
    LPOVERLAPPED overlapped)
{
  ClientConnection *conn = (ClientConnection *) overlapped;
  Win32IpcPipeClient *self = conn->self;
  char mmf_name[1024] = { '\0', };
  Win32IpcVideoInfo info;
  Win32IpcMmf *mmf;
  MmfInfo minfo;

  if (error_code != ERROR_SUCCESS) {
    std::string msg = win32_ipc_error_message (error_code);
    self->last_err = error_code;
    GST_WARNING ("HAVE-DATA failed with 0x%x (%s)",
        self->last_err, msg.c_str ());
    goto error;
  }

  if (!win32_ipc_pkt_parse_have_data (conn->server_msg, n_bytes,
      &conn->seq_num, mmf_name, &info)) {
    self->last_err = ERROR_BAD_FORMAT;
    GST_WARNING ("Couldn't parse HAVE-DATA pkg");
    goto error;
  }

  mmf = win32_ipc_mmf_open (info.size, mmf_name);
  if (!mmf) {
    GST_ERROR ("Couldn't open file %s", mmf_name);
    self->last_err = ERROR_BAD_FORMAT;
    goto error;
  }

  GST_TRACE ("Got HAVE-DATA %s", mmf_name);

  minfo.mmf = mmf;
  minfo.info = info;

  {
    std::lock_guard<std::mutex> lk (self->lock);
    /* Drops too old data */
    while (self->queue.size () > 5) {
      MmfInfo info = self->queue.front ();

      self->queue.pop ();
      win32_ipc_mmf_unref (info.mmf);
    }

    self->queue.push (minfo);
    self->cond.notify_all ();
  }

  self->last_err = win32_ipc_pipe_client_send_read_done_async (self);
  if (self->last_err != ERROR_SUCCESS)
    goto error;

  return;

error:
  SetEvent (self->cancellable);
}

static DWORD
win32_ipc_pipe_client_receive_have_data_async (Win32IpcPipeClient * self)
{
  ClientConnection *conn = &self->conn;

  GST_TRACE ("Waiting HAVE-DATA");

  if (!ReadFileEx (conn->pipe, conn->server_msg, CONN_BUFFER_SIZE,
      (OVERLAPPED *) conn, win32_ipc_pipe_client_receive_have_data_finish)) {
    UINT last_err = GetLastError ();
    std::string msg = win32_ipc_error_message (last_err);
    GST_WARNING ("ReadFileEx failed with 0x%x (%s)", last_err, msg.c_str ());
    return last_err;
  }

  return ERROR_SUCCESS;
}

static VOID WINAPI
pipe_clinet_send_need_data_finish (DWORD error_code, DWORD n_bytes,
    LPOVERLAPPED overlapped)
{
  ClientConnection *conn = (ClientConnection *) overlapped;
  Win32IpcPipeClient *self = conn->self;

  if (error_code != ERROR_SUCCESS) {
    std::string msg = win32_ipc_error_message (error_code);
    self->last_err = error_code;
    GST_WARNING ("NEED-DATA failed with 0x%x (%s)",
        self->last_err, msg.c_str ());
    goto error;
  }

  self->last_err = win32_ipc_pipe_client_receive_have_data_async (self);
  if (self->last_err != ERROR_SUCCESS)
    goto error;

  return;

error:
  SetEvent (self->cancellable);
}

static DWORD
win32_ipc_pipe_client_send_need_data_async (Win32IpcPipeClient * self)
{
  ClientConnection *conn = &self->conn;

  conn->to_write = win32_ipc_pkt_build_need_data (conn->client_msg,
      CONN_BUFFER_SIZE, conn->seq_num);
  if (conn->to_write == 0) {
    GST_ERROR ("Couldn't build NEED-DATA pkt");
    return ERROR_BAD_FORMAT;
  }

  GST_TRACE ("Sending NEED-DATA");

  if (!WriteFileEx (conn->pipe, conn->client_msg, conn->to_write,
      (OVERLAPPED *) conn, pipe_clinet_send_need_data_finish)) {
    UINT last_err = GetLastError ();
    std::string msg = win32_ipc_error_message (last_err);
    GST_WARNING ("WriteFileEx failed with 0x%x (%s)", last_err, msg.c_str ());
    return last_err;
  }

  return ERROR_SUCCESS;
}

static VOID
win32_ipc_pipe_client_loop (Win32IpcPipeClient * self)
{
  DWORD mode = PIPE_READMODE_MESSAGE;
  std::unique_lock<std::mutex> lk (self->lock);
  ClientConnection *conn = &self->conn;

  conn->pipe = CreateFileA (self->name.c_str (),
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, nullptr);
  self->last_err = GetLastError ();
  if (conn->pipe == INVALID_HANDLE_VALUE) {
    std::string msg = win32_ipc_error_message (self->last_err);
    GST_WARNING ("CreateFileA failed with 0x%x (%s)", self->last_err,
        msg.c_str ());
    self->cond.notify_all ();
    return;
  }

  if (!SetNamedPipeHandleState (conn->pipe, &mode, nullptr, nullptr)) {
    self->last_err = GetLastError ();
    std::string msg = win32_ipc_error_message (self->last_err);
    GST_WARNING ("SetNamedPipeHandleState failed with 0x%x (%s)",
        self->last_err, msg.c_str ());
    CloseHandle (conn->pipe);
    conn->pipe = INVALID_HANDLE_VALUE;
    self->cond.notify_all ();
    return;
  }

  self->last_err = ERROR_SUCCESS;
  self->cond.notify_all ();
  lk.unlock ();

  /* Once connection is established, send NEED-DATA message to server,
   * and then it will loop NEED-DATA -> HAVE-DATA -> READ-DONE */
  self->last_err = win32_ipc_pipe_client_send_need_data_async (self);
  if (self->last_err != ERROR_SUCCESS)
    goto out;

  do {
    /* Enters alertable thread state and wait for I/O completion event
     * or cancellable event */
    DWORD ret = WaitForSingleObjectEx (self->cancellable, INFINITE, TRUE);
    if (ret == WAIT_OBJECT_0) {
      GST_DEBUG ("Operation cancelled");
      CancelIoEx (conn->pipe, (OVERLAPPED *) &conn);
      break;
    } else if (ret != WAIT_IO_COMPLETION) {
      GST_WARNING ("Unexpected wait return 0x%x", (UINT) ret);
      CancelIoEx (conn->pipe, (OVERLAPPED *) &conn);
      break;
    }
  } while (true);

out:
  if (conn->pipe != INVALID_HANDLE_VALUE)
    CloseHandle (conn->pipe);

  lk.lock ();
  self->last_err = ERROR_OPERATION_ABORTED;
  conn->pipe = INVALID_HANDLE_VALUE;
  self->cond.notify_all ();
}

static BOOL
win32_ipc_pipe_client_run (Win32IpcPipeClient * self)
{
  std::unique_lock<std::mutex> lk (self->lock);

  self->thread = std::make_unique<std::thread>
      (std::thread (win32_ipc_pipe_client_loop, self));
  self->cond.wait (lk);

  if (self->last_err != ERROR_SUCCESS) {
    self->thread->join ();
    self->thread = nullptr;
    return FALSE;
  }

  return TRUE;
}

Win32IpcPipeClient *
win32_ipc_pipe_client_new (const char * pipe_name)
{
  Win32IpcPipeClient *self;

  if (!pipe_name) {
    GST_ERROR ("Pipe name must be specified");
    return nullptr;
  }

  self = new Win32IpcPipeClient (pipe_name);

  if (!win32_ipc_pipe_client_run (self)) {
    win32_ipc_pipe_client_unref (self);
    return nullptr;
  }

  return self;
}

Win32IpcPipeClient *
win32_ipc_pipe_client_ref (Win32IpcPipeClient * client)
{
  InterlockedIncrement (&client->ref_count);

  return client;
}

void
win32_ipc_pipe_client_unref (Win32IpcPipeClient * client)
{
  ULONG ref_count;

  ref_count = InterlockedDecrement (&client->ref_count);
  if (ref_count == 0)
    delete client;
}

void
win32_ipc_pipe_client_shutdown (Win32IpcPipeClient * client)
{
  GST_DEBUG ("Shutting down %p", client);

  SetEvent (client->cancellable);
  if (client->thread) {
    client->thread->join ();
    client->thread = nullptr;
  }

  std::lock_guard<std::mutex> lk (client->lock);
  client->last_err = ERROR_OPERATION_ABORTED;
  while (!client->queue.empty ()) {
    MmfInfo info = client->queue.front ();

    client->queue.pop ();
    win32_ipc_mmf_unref (info.mmf);
  }
  client->cond.notify_all ();
}

BOOL
win32_ipc_pipe_client_get_mmf (Win32IpcPipeClient * client, Win32IpcMmf ** mmf,
    Win32IpcVideoInfo * info)
{
  std::unique_lock<std::mutex> lk (client->lock);
  if (client->last_err != ERROR_SUCCESS) {
    GST_WARNING ("Last error code was 0x%x", client->last_err);
    return FALSE;
  }

  while (client->queue.empty () && client->last_err == ERROR_SUCCESS)
    client->cond.wait (lk);

  if (client->last_err != ERROR_SUCCESS || client->queue.empty ())
    return FALSE;

  MmfInfo mmf_info = client->queue.front ();
  client->queue.pop ();

  *mmf = mmf_info.mmf;
  *info = mmf_info.info;

  return TRUE;
}
