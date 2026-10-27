#include "wls/cleanup.hpp"
#include "wls/workspace.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <sys/stat.h>
using namespace wls::s1;
using O=boost::json::object;
int main(){
  if(::geteuid()==0) return 9;
  ::umask(0077);
  char pattern[]="/tmp/wls-owned-capacity-red-XXXXXX";
  const char* tmp=::mkdtemp(pattern); if(!tmp) return 8;
  const std::filesystem::path parent(tmp);
  int result=1;
  try {
    const auto id=new_id("res");
    Workspace parent_ws(parent,WorkspaceAccess::ReadOnly);
    const Json intent=O{{"resource_id",id},{"leaf",id},{"request_id","req_fixture"},{"team_id","team_fixture"},
      {"parent_identity",parent_ws.root_identity()},{"marker",O{{"resource_id",id},{"request_id","req_fixture"},
      {"team_id","team_fixture"},{"nonce",new_id("nonce")}}}};
    const Json resource=O{{"resource_id",id},{"intent",intent},{"identity",create_owned_workspace(intent)}};
    const auto work=parent/id/"work";
    for(int i=0;i<4096;++i){std::ofstream f(work/("item-"+std::to_string(i))); f<<"x"; if(!f.good()) return 7;}
    Workspace writer(work,WorkspaceAccess::ReadWrite);
    const auto write=writer.write_atomic("legal-4097th","x");
    if(write.status!=WorkspaceMutationStatus::Succeeded) return 6;
    try{(void)inventory_owned_workspace(resource);}
    catch(const Error& e){
      if(e.code()==ErrorCode::Quota && std::filesystem::exists(work/"legal-4097th")){
        std::cout<<"REPRODUCED uid="<<::geteuid()<<" workspace_default_entries=20000 legal_tree_entries=4097 cleanup_default_entries=4096 inventory_error="<<error_name(e.code())<<" deletion_performed=false\n";
        result=0;
      }
    }
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';}
  std::filesystem::remove_all(parent);return result;
}
