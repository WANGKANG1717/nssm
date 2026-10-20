function(svp_configure_compiler target)
  target_compile_options(${target} PRIVATE
    -Wall
    -Wextra
    -Wformat=2
    -fstack-protector-strong
    -finput-charset=UTF-8
    -fexec-charset=UTF-8
  )

  target_link_options(${target} PRIVATE
    -municode
    -static
    -static-libgcc
    -static-libstdc++
    -Wl,--no-insert-timestamp
    -Wl,--dynamicbase
    -Wl,--nxcompat
    -Wl,--high-entropy-va
  )
endfunction()
