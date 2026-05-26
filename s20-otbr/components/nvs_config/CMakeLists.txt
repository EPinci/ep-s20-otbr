set(requires nvs_flash mbedtls)

# For IDF < 6.0, use IDF's built-in json component; for IDF >= 6.0 use managed cJSON component through idf_component.yml
if("${IDF_VERSION_MAJOR}" VERSION_LESS "6")
    list(APPEND requires json)
endif()

idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    REQUIRES ${requires}
)
