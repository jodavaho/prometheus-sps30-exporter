#include <iostream>
#include <libconfig.h>
#include <fstream>
#include <string>
#include <array>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <sstream>
#include "SimpleWebServer/server_http.hpp"


/** Simple function to get the file at `location`, and writes it into
 * `out_val`.
 *
 * It doesn't do anything interesting, just reads the first line and parses it
 * as a float. This is only really useful at this time for the iio device nodes
 * that sps30 uses. See the config. It's not a bad idea to generalize this a
 * little. 
 *
 */
int get(std::string location, float* out_val){
  std::ifstream file(location.c_str());

  if (!file){
    std::cerr<<"Error opening:"<<location<<std::endl;
    return 2;
  } 

  file >> *out_val;
  
  if (!file){
    std::cerr<<"Unable to read value from file:"<<location<<std::endl;
    return 3;
  }

  file.close();
  return 0;
}

int main (){
  int sleep_s;
  int port;
  std::vector<std::string> endpoints;
  std::vector<std::string> keynames;
  std::vector<std::string> values;

  //I like the C API better, but libconfig is really good. 
  //
  //Find the config file first
  config_t config; config_init(&config);
  if (config_read_file(&config, "/etc/piaq/piaq.conf") != CONFIG_TRUE)
  {
    std::cerr<<"problem in config file!"<<std::endl;
    std::cerr
      <<config_error_file(&config)
      <<":"<<config_error_line(&config)
      <<" \""<<config_error_text(&config)<<"\""
      <<std::endl;
    return 1;
  }

  //Load port fron config:
  auto config_port = config_lookup(&config,"port");
  if (!config_port){
    std::cerr<<"'port' not found in config. Defaulting to 8050"<<std::endl;
    port = 8050;
  } else {
    auto port_desired =  config_setting_get_int(config_port) ;
    if (port_desired==0){
      std::cerr<<"'port'="<<port_desired<<" not valid!"<<std::endl;
      return 1;
    }
    port = port_desired;
  }

  //Load sleep time from config:
  auto config_sleep = config_lookup(&config,"sleep");
  if (!config_sleep){
    std::cerr<<"'sleep' not found in config. Defaulting to 5s"<<std::endl;
    sleep_s = 5;
  } else {
    auto sleep_desired =  config_setting_get_int(config_sleep) ;
    if (sleep_desired == 0){
      //might have failed to parse an int, maybe it's a float?
      sleep_desired = std::floor(config_setting_get_float(config_sleep));
    }
    std::cerr<<"Read: sleep:"<<sleep_desired<<std::endl;
    if (sleep_desired==0){
      std::cerr<<"'sleep'="<<sleep_desired<<" not valid!"<<std::endl;
      return 1;
    }
    sleep_s = sleep_desired;
  }
  

  //Load Endpoints:
  auto config_endpoints= config_lookup(&config,"endpoints");
  if (!config_endpoints){
    std::cerr<<"endpoints not found in config file!"<<std::endl;
    return 1;
  }

  //the endpoints are specified as a list of tuples. Get the elements from the list by querying them in 0...N order. 
  int elem_counter=0;
  config_setting_t *config_endpoint_i;
  while ( (config_endpoint_i = config_setting_get_elem(config_endpoints,elem_counter)) ){

    std::cerr<<"Setting up endpoint "<<elem_counter<<std::endl;

    //grab the elements of each tuple ("key" first)
    auto key_name_ptr = config_setting_get_member(config_endpoint_i,"key");
    if (!key_name_ptr){
      std::cerr<<"Could not locate key name in config!"<<std::endl;
      return 1;
    }
    //get value of "key", which is the name of the prometheus data to populate
    auto key_name = config_setting_get_string(key_name_ptr);
    if (!key_name){
      std::cerr<<"Could not locate key name in config!"<<std::endl;
      return 1;
    }

    //Now "node"
    auto endpoint_ptr = config_setting_get_member(config_endpoint_i,"node");
    if (!endpoint_ptr){
      std::cerr<<"Could not find node location in config!"<<std::endl;
      return 1;
    }

    //and the value of the element "node", which is a filename, really. Thanks, Linux.
    auto node = config_setting_get_string(endpoint_ptr);
    if (!key_name){
      std::cerr<<"Could not find node location in config!"<<std::endl;
      return 1;
    }

    endpoints.push_back(node);
    keynames.push_back(key_name);
    values.push_back("0");
    
    elem_counter++;
  }
  
  //good enough, close it. 
  config_destroy(&config);

  //here's where we'll fetch the data to, so it can be served on demand
  std::unordered_map<std::string,std::string> kvpairs;

  //Now, set up webserver to serve the data
  using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;

  HttpServer server;
  server.config.port=port;

  //set up a listener for "/" requests.
  server.resource["^/$"]["GET"]=[&keynames,&values](std::shared_ptr<HttpServer::Response> response, std::shared_ptr<HttpServer::Request> request){

    std::stringstream ss;

    //create openmetrics formatted output for each piece of data found in the node we're configured to read
    for (size_t i=0;i<keynames.size();i++){
      ss<<"# TYPE sps30_particle_gauge_"<< keynames[i]<<" gauge"<<std::endl;
      ss<<"# HELP sps30_particle_gauge_"<< keynames[i]<<" \"The mass concentration measured in micrograms per cubic meter.\""<<std::endl;
      ss<<"sps30_particle_gauge_"<< keynames[i]<<" "<<values[i]<<std::endl;
    }
    ss<<"# eof"<<std::endl;

    std::cerr << request->remote_endpoint().address().to_string() << ":" << request->remote_endpoint().port()<<std::endl;
    std::cerr << request->method << " " << request->path << " HTTP/" << request->http_version <<std::endl;

    response->write(ss);
  };


  //and start the server in the background
  std::promise<unsigned short> server_port;
  std::thread server_thread([&server, &server_port]() {
    // Start server
    server.start([&server_port](unsigned short port) {
      server_port.set_value(port);
    });
  });
  std::cout << "Server listening on port " << server_port.get_future().get() << std::endl;

  //this thread will go on and periodically read the values for the background server to serve.
  while(1){
    std::this_thread::sleep_for(std::chrono::seconds(sleep_s));
    std::cerr<<"Reading ... "<<std::endl;
    for (size_t i=0;i<endpoints.size();i++){
      float f;

      //read the node into the float f
      if (get(endpoints[i],&f)!=0){
        std::cerr<<"error reading file!"<<std::endl;
      } else {
        values[i]=std::to_string(f);
        std::cout<<endpoints[i]<<":"<<values[i]<<std::endl;
      }
    }
  }

  return 0;
}
