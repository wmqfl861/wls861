#include "wls/cleanup.hpp"
#include "wls/workspace.hpp"
#include "test_support.hpp"
#include <fstream>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>

using namespace wls::s1;
using O=boost::json::object;
namespace {
struct Fixture {
    test::TempDir temp;
    Json intent, resource;
    std::filesystem::path envelope,work;
    Fixture(){
        Workspace parent(temp.path,WorkspaceAccess::ReadOnly);
        const auto id=new_id("res");
        intent=O{{"resource_id",id},{"leaf",id},{"request_id","req_fixture"},{"team_id","team_fixture"},
            {"parent_identity",parent.root_identity()}, {"marker",O{{"resource_id",id},{"request_id","req_fixture"},
            {"team_id","team_fixture"},{"nonce",new_id("nonce")}}}};
        resource=O{{"resource_id",id},{"intent",intent},{"identity",create_owned_workspace(intent)}};
        envelope=temp.path/id;work=envelope/"work";
    }
    void write(const std::string& path,const std::string& value="hello"){
        std::ofstream out(work/path,std::ios::binary);out<<value; CHECK(out.good());
    }
};
void complete_and_repeat(){Fixture f;f.write("one");std::filesystem::create_directory(f.work/"nested");f.write("nested/two");
    const auto inv=inventory_owned_workspace(f.resource);CHECK(inv.at("entries").as_array().size()==3);
    const auto done=erase_owned_workspace(f.resource,inv);CHECK(done.at("complete").as_bool());
    CHECK(!std::filesystem::exists(f.work));CHECK(std::filesystem::exists(f.envelope/"owner.json"));
    CHECK(erase_owned_workspace(f.resource,inv).at("complete").as_bool());
    CHECK(verify_owned_workspace_removed(f.resource,inv).at("durable").as_bool());}
void resume_bounded(){Fixture f;f.write("a");f.write("b");const auto inv=inventory_owned_workspace(f.resource);
    CHECK(!erase_owned_workspace(f.resource,inv,1).at("complete").as_bool());
    CHECK(!erase_owned_workspace(f.resource,inv,1).at("complete").as_bool());
    CHECK(erase_owned_workspace(f.resource,inv,1).at("complete").as_bool());}
void empty(){Fixture f;CHECK(create_owned_workspace(f.intent)==f.resource.at("identity"));
    CHECK(erase_owned_workspace(f.resource,inventory_owned_workspace(f.resource)).at("complete").as_bool());}
void no_adoption(){Fixture f;f.write("keep");test::throws<Error>([&]{(void)create_owned_workspace(f.intent);});CHECK(std::filesystem::exists(f.work/"keep"));}
void new_entry(){Fixture f;f.write("z");const auto inv=inventory_owned_workspace(f.resource);f.write("a");
    test::throws<Error>([&]{(void)erase_owned_workspace(f.resource,inv);});CHECK(std::filesystem::exists(f.work/"z"));}
void changed_entry(){Fixture f;f.write("z");f.write("a");const auto inv=inventory_owned_workspace(f.resource);f.write("a","changed");
    test::throws<Error>([&]{(void)erase_owned_workspace(f.resource,inv);});CHECK(std::filesystem::exists(f.work/"z"));}
void replaced_entry(){Fixture f;f.write("one");const auto inv=inventory_owned_workspace(f.resource);
    std::filesystem::rename(f.work/"one",f.temp.path/"original");f.write("one");
    test::throws<Error>([&]{(void)erase_owned_workspace(f.resource,inv);});CHECK(std::filesystem::exists(f.temp.path/"original"));}
void symlink_test(){Fixture f;f.write("safe");std::filesystem::create_symlink(f.temp.path,f.work/"link");
    test::throws<Error>([&]{(void)inventory_owned_workspace(f.resource);});CHECK(std::filesystem::exists(f.work/"safe"));}
void hardlink(){Fixture f;f.write("one");std::filesystem::create_hard_link(f.work/"one",f.temp.path/"other");
    test::throws<Error>([&]{(void)inventory_owned_workspace(f.resource);});}
void fifo(){Fixture f;CHECK(::mkfifo((f.work/"pipe").c_str(),0600)==0);test::throws<Error>([&]{(void)inventory_owned_workspace(f.resource);});}
void marker_tamper(){Fixture f;const auto inv=inventory_owned_workspace(f.resource);{std::ofstream o(f.envelope/"owner.json");o<<"wrong";}
    test::throws<Error>([&]{(void)erase_owned_workspace(f.resource,inv);});CHECK(std::filesystem::exists(f.work));}
void root_replaced(){Fixture f;const auto inv=inventory_owned_workspace(f.resource);std::filesystem::rename(f.work,f.temp.path/"old");
    std::filesystem::create_directory(f.work);test::throws<Error>([&]{(void)erase_owned_workspace(f.resource,inv);});CHECK(std::filesystem::exists(f.temp.path/"old"));}
void envelope_replaced(){Fixture f;const auto inv=inventory_owned_workspace(f.resource);std::filesystem::rename(f.envelope,f.temp.path/"old");
    std::filesystem::create_directory(f.envelope);test::throws<Error>([&]{(void)erase_owned_workspace(f.resource,inv);});}
void parent_replaced(){Fixture f;const auto inv=inventory_owned_workspace(f.resource);const auto moved=f.temp.path.string()+"-moved";
    std::filesystem::rename(f.temp.path,moved);std::filesystem::create_directory(f.temp.path);
    test::throws<Error>([&]{(void)erase_owned_workspace(f.resource,inv);});std::filesystem::remove(f.temp.path);std::filesystem::rename(moved,f.temp.path);}
void limits(){Fixture f;f.write("one","0123456789");CleanupLimits bound;bound.max_file_bytes=2;
    test::throws<Error>([&]{(void)inventory_owned_workspace(f.resource,bound);});
    bound.max_file_bytes=16U*1024U*1024U;bound.max_total_bytes=9;
    test::throws<Error>([&]{(void)inventory_owned_workspace(f.resource,bound);});
    const auto inv=inventory_owned_workspace(f.resource);test::throws<Error>([&]{(void)erase_owned_workspace(f.resource,inv,0);});}
void nested_limit(){Fixture f;std::filesystem::create_directory(f.work/"sub");f.write("sub/one");CleanupLimits lim;lim.max_depth=1;
    test::throws<Error>([&]{(void)inventory_owned_workspace(f.resource,lim);});lim.max_depth=32;lim.max_entries=1;
    test::throws<Error>([&]{(void)inventory_owned_workspace(f.resource,lim);});}
void lock_conflict(){Fixture f;const int fd=::open(f.work.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);CHECK(fd>=0);CHECK(::flock(fd,LOCK_EX|LOCK_NB)==0);
    test::throws<Error>([&]{(void)inventory_owned_workspace(f.resource);});CHECK(::close(fd)==0);CHECK(inspect_owned_workspace(f.resource)==f.resource.at("identity"));}
void wrong_inventory(){Fixture f;auto inv=inventory_owned_workspace(f.resource);inv.as_object()["resource_id"]="res_other";
    test::throws<Error>([&]{(void)erase_owned_workspace(f.resource,inv);});CHECK(std::filesystem::exists(f.work));}
void recovered_new_file(){Fixture f;f.write("a");f.write("z");const auto inv=inventory_owned_workspace(f.resource);
    CHECK(!erase_owned_workspace(f.resource,inv,1).at("complete").as_bool());f.write("new");
    test::throws<Error>([&]{(void)erase_owned_workspace(f.resource,inv);});CHECK(std::filesystem::exists(f.work/"a"));}
void verify_not_delete(){Fixture f;const auto inv=inventory_owned_workspace(f.resource);test::throws<Error>([&]{(void)verify_owned_workspace_removed(f.resource,inv);});
    CHECK(std::filesystem::exists(f.work));}
}
int main(){return test::run({{"original bytes deleted and exact repeat",complete_and_repeat},{"bounded restart",resume_bounded},
    {"empty owned directory",empty},{"no populated adoption",no_adoption},{"new entry preflight",new_entry},{"changed byte preflight",changed_entry},
    {"same byte replacement refused",replaced_entry},{"symlink refused",symlink_test},{"hardlink refused",hardlink},{"special file refused",fifo},
    {"receipt tamper refused",marker_tamper},{"workspace replacement",root_replaced},{"envelope replacement",envelope_replaced},
    {"parent replacement",parent_replaced},{"byte and pass limits",limits},{"depth and count bounds",nested_limit},
    {"workspace lock conflict",lock_conflict},{"wrong inventory",wrong_inventory},{"new file after partial cleanup",recovered_new_file},
    {"verification never deletes",verify_not_delete}});}
