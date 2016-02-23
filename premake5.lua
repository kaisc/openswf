workspace("openswf")
    configurations({ "debug", "release" })
    location("build")
    buildoptions({"-std=c++11", "-stdlib=libc++"})

    filter("configurations:debug")
        defines({"DEBUG"})
        flags({"Symbols"})

    filter("configurations:release")
        defines({"NDEBUG"})
        optimize("On")

    project("unit-test")
        kind("ConsoleApp")
        language("C++")
        location("build/unit-test")
        files({"test/**.cpp", "source/**.cpp"})
        includedirs({ "3rd/catch/include", "source" })