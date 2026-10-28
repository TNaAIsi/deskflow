// SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
// SPDX-License-Identifier: BSL-1.0
// Deskflow adapter: acknowledge a report only after the driver service replies.
// Applied to the build copy of the unmodified upstream client header.
  template <typename Report>
  void async_post_report_with_completion(const Report& report, std::function<void(bool)> completion) {
    request type;
    if constexpr (std::is_same_v<Report, virtual_hid_device_driver::hid_report::keyboard_input>) {
      type = request::post_keyboard_input_report;
    } else {
      static_assert(std::is_same_v<Report, virtual_hid_device_driver::hid_report::consumer_input>);
      type = request::post_consumer_input_report;
    }
    enqueue_to_dispatcher([this, type, report, completion] {
      if (!client_ || !connected_) {
        completion(false);
        return;
      }
      auto buffer = make_request_buffer(type, report);
      client_->async_request(*buffer, [this, completion](auto error, auto response_buffer) {
        enqueue_to_dispatcher([this, completion, error, response_buffer] {
          if (!error) {
            handle_message(response_buffer);
          }
          completion(!error);
        });
      });
    });
  }
