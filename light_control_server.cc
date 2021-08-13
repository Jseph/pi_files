#include <pigpio.h>

#include <boost/beast/core.hpp>
#include <boost/beast/core/ostream.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/fiber/all.hpp>
#include <boost/fiber/unbuffered_channel.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using tcp = boost::asio::ip::tcp;
namespace websocket = boost::beast::websocket;

//------------------------------------------------------------------------------

// Report a failure
void fail(boost::system::error_code ec, char const* what) {
  std::cerr << what << ": " << ec.message() << "\n";
}

class Session;

struct State {
  int x;
  int y;
  int brightness;
  int r;
  int g;
  int b;
  Session* session;
};

// Echoes back all received WebSocket messages
class Session : public std::enable_shared_from_this<Session>
{
  bool open_;
  websocket::stream<tcp::socket> ws_;
  boost::asio::strand<
    boost::asio::io_context::executor_type> strand_;
  boost::beast::multi_buffer buffer_;
  boost::fibers::unbuffered_channel<State>* output_channel_;
  boost::mutex mu_;
  bool write_active_;
  public:
  // Take ownership of the socket
  explicit
    Session(tcp::socket socket, 
        boost::fibers::unbuffered_channel<State>* channel)
    : open_(true),
    ws_(std::move(socket)), 
    strand_(ws_.get_executor()),
    output_channel_(channel),
    write_active_(false){}

  bool isOpen() {
    return open_;
  }

  // Start the asynchronous operation
  void run(std::string first_message) {
    // Accept the websocket handshake
    ws_.async_accept(
        boost::asio::bind_executor(
          strand_,
          std::bind(
            &Session::on_accept,
            shared_from_this(),
            first_message,
            std::placeholders::_1)));
  }

  void on_accept(std::string first_message, 
      boost::system::error_code ec) {
    if(ec) return fail(ec, "accept");
    try_write(first_message);
    // Start read and write loops.
    do_read();
  }

  void do_read() {
    // Read a message into our buffer
    ws_.async_read(
        buffer_,
        boost::asio::bind_executor(
          strand_,
          std::bind(
            &Session::on_read,
            shared_from_this(),
            std::placeholders::_1,
            std::placeholders::_2)));
  }

  void on_read(
      boost::system::error_code ec,
      std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    // This indicates that the session was closed
    if(ec){
      if(ec != websocket::error::closed) {
        fail(ec, "read");
      }
      open_ = false;
      return;
    }

    State state;
    std::stringstream message;
    message << boost::beast::buffers(buffer_.data());
    message >> state.x >> state.y >> state.brightness >>
      state.r >> state.g >> state.b;
    state.session = this;
    output_channel_->push(state);

    // Clear the buffer
    buffer_.consume(buffer_.size());
    do_read();
  }

  void try_write(const std::string& message) {
    boost::mutex::scoped_lock l(mu_);
    if (!write_active_) {
      write_active_ = true;
      do_write(message);
    }
  }

  // Call try_write.  Don't call do_write directly.
  void do_write(const std::string& message) {
    ws_.text(true);
    ws_.async_write(
        boost::asio::buffer(message),
        boost::asio::bind_executor(
          strand_,
          std::bind(
            &Session::on_write,
            shared_from_this(),
            std::placeholders::_1,
            std::placeholders::_2)));
  }

  void on_write(
      boost::system::error_code ec,
      std::size_t bytes_transferred) {
    boost::ignore_unused(bytes_transferred);

    if(ec) {
      fail(ec, "write");
    }
    boost::mutex::scoped_lock l(mu_);
    write_active_ = false;
  }
};

//------------------------------------------------------------------------------

// Accepts incoming connections and launches the sessions
class listener : public std::enable_shared_from_this<listener>
{
  tcp::acceptor acceptor_;
  tcp::socket socket_;
  std::vector<std::shared_ptr<Session>> sessions_;
  boost::fibers::unbuffered_channel<State> channel_;
  boost::fibers::fiber read_loop_;
  int x_, y_, brightness_;
  public:
  listener(
      boost::asio::io_context& ioc,
      tcp::endpoint endpoint)
    : acceptor_(ioc) , socket_(ioc), x_(0),
    y_(0), brightness_(0) {
      boost::system::error_code ec;

      // Open the acceptor
      acceptor_.open(endpoint.protocol(), ec);
      if(ec) {
        fail(ec, "open");
        return;
      }

      // Bind to the server address
      acceptor_.bind(endpoint, ec);
      if(ec) {
        fail(ec, "bind");
        return;
      }

      // Start listening for connections
      acceptor_.listen(
          boost::asio::socket_base::max_listen_connections, ec);
      if(ec) {
        fail(ec, "listen");
        return;
      }
    }
  void read_loop() {
    while(true) {
      State state = channel_.value_pop();
      // Do some rudamentery white balancing.
      int r = state.r * 2;
      int g = state.g;
      int b = state.b;
      if (r > 255) {
        g = g * 255 / r;
        b = b * 255 / r;
        r = 255;
      }
      gpioPWM(17, g);
      gpioPWM(22, b);
      gpioPWM(27, r);
      /*std::cout << "r: " << state.r << " g: " << state.g 
        << " b: " << state.b << "\n";*/
      std::ostringstream output;
      x_ = state.x;
      y_ = state.y;
      brightness_ = state.brightness;
      output << x_ << " " << y_ << " " << brightness_;
      for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (!(*it)->isOpen()) {
          sessions_.erase(it);
        } else {
          if ((*it).get() != state.session) {
            (*it)->try_write(output.str());
          }
          it++;
        }
      }
    }
  };
  // Start accepting incoming connections
  void run() {
    if(!acceptor_.is_open()) {
      return;
    }
    do_accept();
    boost::fibers::fiber f([this](){read_loop();});
    read_loop_ = std::move(f);
  }

  void do_accept() {
    acceptor_.async_accept(
        socket_,
        std::bind(
          &listener::on_accept,
          shared_from_this(),
          std::placeholders::_1));
  }

  void on_accept(boost::system::error_code ec) {
    if(ec) {
      fail(ec, "accept");
    }
    else {
      // Create the session and run it
      sessions_.push_back(std::make_shared<Session>(
            std::move(socket_), &channel_));
      std::ostringstream output;
      output << x_ << " " << y_ << " " << brightness_;
      sessions_.back()->run(output.str());
    }

    // Accept another connection
    do_accept();
  }
};

//------------------------------------------------------------------------------

int main(int argc, char* argv[]) {
  const auto address = boost::asio::ip::make_address("0.0.0.0");
  const auto port = static_cast<unsigned short>(1337);

  // The io_context is required for all I/O
  boost::asio::io_context ioc{1};

  if (gpioInitialise() < 0) return -1;
  gpioPWM(17, 0);
  gpioPWM(22, 0);
  gpioPWM(27, 0);

  // Create and launch a listening port
  std::make_shared<listener>(ioc, tcp::endpoint{address, port})->run();

  // Run the I/O service on the requested number of threads
  ioc.run();
  return EXIT_SUCCESS;
}
