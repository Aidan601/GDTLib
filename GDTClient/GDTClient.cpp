#include <iostream>
#include "GDT.h"

int main()
{
    gdt my("p7_54i_gear_set");
    std::cout << my.ToString() << "\n";

    // Build absolute path from env (won't print if env exists)
    std::string abs = my.GetAbsPath();
    if (!abs.empty())
        std::cout << "AbsPath: " << abs << "\n";

    // Add two assets of different types
    xasset a1("i_example_01_c", XType::Accolade);
    xasset a2("list_example", XType::AccoladeList);

    bool inserted1 = my.AddXAsset(a1);
    bool inserted2 = my.AddXAsset(a2);

    std::cout << "Inserted a1? " << (inserted1 ? "true" : "false") << "\n";
    std::cout << "Inserted a2? " << (inserted2 ? "true" : "false") << "\n";

    // Lookup by name
    if (auto *found = my.GetXAsset("i_example_01_c"))
        std::cout << "Found: " << found->ToString() << "\n";

    // Counts
    std::cout << "Accolades:    " << my.Count(XType::Accolade) << "\n";
    std::cout << "AccoladeList: " << my.Count(XType::AccoladeList) << "\n";
    std::cout << "Total:        " << my.TotalCount() << "\n";

    // Replace/migrate: change type for existing name
    xasset a1_migrated("i_example_01_c", XType::AccoladeList);
    bool inserted3 = my.AddXAsset(a1_migrated); // replace; returns false
    std::cout << "Replaced a1 as AccoladeList? " << (!inserted3 ? "true" : "false") << "\n";

    std::cout << "Accolades:    " << my.Count(XType::Accolade) << "\n";
    std::cout << "AccoladeList: " << my.Count(XType::AccoladeList) << "\n";
    std::cout << "Total:        " << my.TotalCount() << "\n";

    // Remove
    bool removed = my.RemoveByName("i_example_01_c");
    std::cout << "Removed i_example_01_c? " << (removed ? "true" : "false") << "\n";

    std::cout << "Accolades:    " << my.Count(XType::Accolade) << "\n";
    std::cout << "AccoladeList: " << my.Count(XType::AccoladeList) << "\n";
    std::cout << "Total:        " << my.TotalCount() << "\n";

    return 0;
}
